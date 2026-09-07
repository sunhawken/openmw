#include "seamwelder.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <unordered_map>

#include <osg/Array>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/NodeVisitor>

#include <components/debug/debuglog.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/settings/values.hpp>

namespace MWRender
{
    namespace
    {
        // Bones the jiggle system drives. A mesh weighted to any of these is a
        // "jiggle-driven" mesh; the seams we care about are where a mesh NOT
        // weighted to them meets one that is.
        constexpr std::array<std::string_view, 4> sJiggleBones = {
            "bip01 l breast",
            "bip01 r breast",
            "bip01 l butt",
            "bip01 r butt",
        };

        bool isJiggleMesh(const SceneUtil::RigGeometry& rig)
        {
            for (const std::string& bone : rig.getInfluenceBoneNames())
                for (std::string_view jb : sJiggleBones)
                    if (bone == jb)
                        return true;
            return false;
        }

        const osg::Vec3Array* sourceVertices(const SceneUtil::RigGeometry& rig)
        {
            osg::ref_ptr<osg::Geometry> src = rig.getSourceGeometry();
            if (!src)
                return nullptr;
            return dynamic_cast<const osg::Vec3Array*>(src->getVertexArray());
        }

        // Collects every RigGeometry under a subgraph.
        class RigCollector : public osg::NodeVisitor
        {
        public:
            RigCollector()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Drawable& drawable) override
            {
                if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                    mRigs.push_back(rig);
            }

            std::vector<SceneUtil::RigGeometry*> mRigs;
        };

        // Simple uniform-grid spatial hash for nearest-vertex-within-threshold queries.
        class VertexGrid
        {
        public:
            explicit VertexGrid(float cellSize)
                : mCellSize(cellSize > 0.f ? cellSize : 1.f)
            {
            }

            void insert(const osg::Vec3f& pos, SceneUtil::RigGeometry* rig, std::size_t index)
            {
                mCells[key(pos)].push_back(Entry{ pos, rig, index });
            }

            struct Entry
            {
                osg::Vec3f mPos;
                SceneUtil::RigGeometry* mRig;
                std::size_t mIndex;
            };

            // Nearest inserted entry to pos within threshold, or nullptr.
            const Entry* nearest(const osg::Vec3f& pos, float threshold) const
            {
                const float t2 = threshold * threshold;
                const Entry* best = nullptr;
                float bestDist = t2;
                const std::int64_t cx = cell(pos.x());
                const std::int64_t cy = cell(pos.y());
                const std::int64_t cz = cell(pos.z());
                for (std::int64_t dx = -1; dx <= 1; ++dx)
                    for (std::int64_t dy = -1; dy <= 1; ++dy)
                        for (std::int64_t dz = -1; dz <= 1; ++dz)
                        {
                            auto it = mCells.find(hash(cx + dx, cy + dy, cz + dz));
                            if (it == mCells.end())
                                continue;
                            for (const Entry& e : it->second)
                            {
                                const float d2 = (e.mPos - pos).length2();
                                if (d2 <= bestDist)
                                {
                                    bestDist = d2;
                                    best = &e;
                                }
                            }
                        }
                return best;
            }

        private:
            std::int64_t cell(float v) const { return static_cast<std::int64_t>(std::floor(v / mCellSize)); }

            std::uint64_t hash(std::int64_t x, std::int64_t y, std::int64_t z) const
            {
                // Cheap 3D hash combine.
                std::uint64_t h = static_cast<std::uint64_t>(x) * 73856093u;
                h ^= static_cast<std::uint64_t>(y) * 19349663u;
                h ^= static_cast<std::uint64_t>(z) * 83492791u;
                return h;
            }

            std::uint64_t key(const osg::Vec3f& pos) const { return hash(cell(pos.x()), cell(pos.y()), cell(pos.z())); }

            float mCellSize;
            std::unordered_map<std::uint64_t, std::vector<Entry>> mCells;
        };
    }

    void SeamWelder::clear()
    {
        mPairs.clear();
        mReferenced.clear();
    }

    void SeamWelder::build(osg::Group* objectRoot)
    {
        clear();

        if (!objectRoot || !Settings::game().mJiggleSeamWelding)
            return;

        const bool debug = Settings::game().mJiggleSeamWeldDebug;
        const float threshold = Settings::game().mJiggleSeamWeldThreshold;

        RigCollector collector;
        objectRoot->accept(collector);

        std::vector<SceneUtil::RigGeometry*> jiggleMeshes;
        std::vector<SceneUtil::RigGeometry*> rigidMeshes;
        for (SceneUtil::RigGeometry* rig : collector.mRigs)
        {
            if (isJiggleMesh(*rig))
                jiggleMeshes.push_back(rig);
            else
                rigidMeshes.push_back(rig);
        }

        if (debug)
            Log(Debug::Warning) << "Seam welder: scanned " << collector.mRigs.size() << " rigged meshes ("
                                << jiggleMeshes.size() << " jiggle, " << rigidMeshes.size() << " rigid)";

        if (jiggleMeshes.empty() || rigidMeshes.empty())
            return;

        // Index all jiggle-mesh rest-pose vertices into a spatial grid.
        VertexGrid grid(threshold);
        for (SceneUtil::RigGeometry* rig : jiggleMeshes)
        {
            const osg::Vec3Array* verts = sourceVertices(*rig);
            if (!verts)
                continue;
            for (std::size_t i = 0; i < verts->size(); ++i)
                grid.insert((*verts)[i], rig, i);
        }

        // For each rigid-mesh vertex, weld it to the coincident jiggle-mesh vertex.
        std::unordered_map<SceneUtil::RigGeometry*, bool> referenced;
        for (SceneUtil::RigGeometry* rig : rigidMeshes)
        {
            const osg::Vec3Array* verts = sourceVertices(*rig);
            if (!verts)
                continue;
            std::size_t meshPairs = 0;
            for (std::size_t i = 0; i < verts->size(); ++i)
            {
                const VertexGrid::Entry* match = grid.nearest((*verts)[i], threshold);
                if (!match)
                    continue;
                mPairs.push_back(WeldPair{ rig, i, match->mRig, match->mIndex });
                referenced[rig] = true;
                referenced[match->mRig] = true;
                ++meshPairs;
            }
            if (debug && meshPairs > 0)
                Log(Debug::Warning) << "Seam welder: rigid mesh welded " << meshPairs << " vertices";
        }

        for (auto& [rig, _] : referenced)
            mReferenced.emplace_back(rig);

        if (debug)
            Log(Debug::Warning) << "Seam welder: built " << mPairs.size() << " weld pairs across "
                                << mReferenced.size() << " meshes";
    }
}
