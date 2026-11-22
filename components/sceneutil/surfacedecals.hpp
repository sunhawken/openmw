#ifndef OPENMW_COMPONENTS_SCENEUTIL_SURFACEDECALS_H
#define OPENMW_COMPONENTS_SCENEUTIL_SURFACEDECALS_H

#include <osg/Geometry>
#include <osg/StateSet>

#include <array>
#include <cstddef>
#include <vector>

namespace osgUtil
{
    class CullVisitor;
}

namespace SceneUtil
{
    // Owned by one rendered surface instance. The source geometry, its arrays,
    // textures and materials are never modified. Call cull after the source has
    // been deformed, in the same local coordinate system and state stack.
    class SurfaceDecals final : public osg::Referenced
    {
    public:
        struct Anchor
        {
            std::array<unsigned int, 3> indices;
            osg::Vec3f weights;
            osg::Vec2f uv;
        };

        struct PatchData
        {
            // Consecutive triples are the clipped output triangles. Every
            // vertex keeps its original source triangle and barycentric weights.
            std::vector<Anchor> vertices;
            osg::ref_ptr<osg::StateSet> stateSet;
            osg::Node::NodeMask mask;
        };

        explicit SurfaceDecals(std::size_t maxPatches = 24, std::size_t maxVerticesPerPatch = 3072);
        SurfaceDecals(const SurfaceDecals&) = delete;
        SurfaceDecals& operator=(const SurfaceDecals&) = delete;

        // StateSet and the objects it references must be treated as immutable
        // while attached, like ordinary shared renderer materials. Mutations
        // belong to scene update, never concurrent with cull traversal.
        bool addPatch(const std::vector<Anchor>& vertices, osg::StateSet* state, osg::Node::NodeMask mask);
        void clear();

        std::size_t patchCount() const { return mPatches.size(); }
        std::size_t vertexCount() const;
        std::size_t evictedPatchCount() const { return mEvicted; }
        const PatchData& getPatch(std::size_t index) const { return mPatches.at(index).data; }

        // Also usable when freezing a patch for a severed/released body part.
        // Output arrays must already have patch.vertices.size() elements.
        static bool evaluate(const osg::Geometry& current, const PatchData& patch,
            osg::Vec3Array& positions, osg::Vec3Array& normals);

        // Double buffering follows RigGeometry/MorphGeometry's draw-thread
        // contract. Never uses a previous-frame source or changes shared assets.
        // A patch's node mask is checked for each view, including shadow views.
        void cull(const osg::Geometry& current, osgUtil::CullVisitor& visitor);

    private:
        struct Patch
        {
            PatchData data;
            std::array<osg::ref_ptr<osg::Geometry>, 2> geometry;
            unsigned int lastTraversal = 0;
            bool initialized = false;
            bool valid = false;
        };

        std::size_t mMaxPatches;
        std::size_t mMaxVerticesPerPatch;
        std::size_t mEvicted = 0;
        std::vector<Patch> mPatches;
    };
}

#endif
