#include "seamwelder.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

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

    namespace
    {
        // Cached rest-pose skin data for a jiggle mesh (read once, reused per rigid vertex).
        struct JiggleMeshData
        {
            std::vector<SceneUtil::RigGeometry::BoneInfo> mBones;
            std::vector<SceneUtil::RigGeometry::BoneWeights> mPerVertex;
        };
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

        // Index all jiggle-mesh rest-pose vertices into a spatial grid, and cache their skin data.
        VertexGrid grid(threshold);
        std::unordered_map<SceneUtil::RigGeometry*, JiggleMeshData> jiggleData;
        for (SceneUtil::RigGeometry* rig : jiggleMeshes)
        {
            const osg::Vec3Array* verts = sourceVertices(*rig);
            if (!verts || verts->empty())
                continue;
            JiggleMeshData& data = jiggleData[rig];
            data.mBones = rig->getBoneInfoList();
            data.mPerVertex = rig->getPerVertexInfluences(verts->size());
            for (std::size_t i = 0; i < verts->size(); ++i)
                grid.insert((*verts)[i], rig, i);
        }

        std::size_t totalWelded = 0;
        // For each rigid mesh, transplant the coincident jiggle vertex's bone weights onto each
        // boundary vertex, so ordinary skinning makes it move with the jiggle mesh (no crack).
        for (SceneUtil::RigGeometry* rig : rigidMeshes)
        {
            const osg::Vec3Array* verts = sourceVertices(*rig);
            if (!verts || verts->empty())
                continue;

            std::vector<SceneUtil::RigGeometry::BoneInfo> bones = rig->getBoneInfoList();
            std::vector<SceneUtil::RigGeometry::BoneWeights> perVertex = rig->getPerVertexInfluences(verts->size());

            // Bone name -> index in this rigid mesh's (growing) bone table.
            std::unordered_map<std::string, std::size_t> boneIndex;
            for (std::size_t b = 0; b < bones.size(); ++b)
                boneIndex.emplace(bones[b].mName, b);

            std::size_t meshWelded = 0;
            for (std::size_t i = 0; i < verts->size(); ++i)
            {
                const VertexGrid::Entry* match = grid.nearest((*verts)[i], threshold);
                if (!match)
                    continue;

                auto dataIt = jiggleData.find(match->mRig);
                if (dataIt == jiggleData.end())
                    continue;
                const JiggleMeshData& src = dataIt->second;
                if (match->mIndex >= src.mPerVertex.size())
                    continue;
                const SceneUtil::RigGeometry::BoneWeights& srcWeights = src.mPerVertex[match->mIndex];
                if (srcWeights.empty())
                    continue;

                // Translate the jiggle vertex's weights into this rigid mesh's bone table,
                // adding any bones (e.g. the jiggle bones) it doesn't already have.
                SceneUtil::RigGeometry::BoneWeights newWeights;
                newWeights.reserve(srcWeights.size());
                for (const auto& [srcBone, weight] : srcWeights)
                {
                    if (srcBone >= src.mBones.size())
                        continue;
                    const SceneUtil::RigGeometry::BoneInfo& info = src.mBones[srcBone];
                    auto found = boneIndex.find(info.mName);
                    std::size_t idx;
                    if (found != boneIndex.end())
                        idx = found->second;
                    else
                    {
                        idx = bones.size();
                        bones.push_back(info);
                        boneIndex.emplace(info.mName, idx);
                    }
                    newWeights.emplace_back(idx, weight);
                }
                if (newWeights.empty())
                    continue;

                perVertex[i] = std::move(newWeights);
                mPairs.push_back(WeldPair{ rig, i, match->mRig, match->mIndex });
                ++meshWelded;
            }

            if (meshWelded > 0)
            {
                rig->setBoneInfo(std::move(bones));
                rig->setInfluences(perVertex);
                rig->reinitialize();
                mReferenced.emplace_back(rig);
                totalWelded += meshWelded;
                if (debug)
                    Log(Debug::Warning) << "Seam welder: welded " << meshWelded << " vertices on a rigid mesh";
            }
        }

        if (debug)
            Log(Debug::Warning) << "Seam welder: welded " << totalWelded << " vertices across "
                                << mReferenced.size() << " meshes";
    }
}
