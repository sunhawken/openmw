#ifndef OPENMW_MWRENDER_LIMBSEVERING_H
#define OPENMW_MWRENDER_LIMBSEVERING_H

#include "decapitation.hpp"
#include <memory>

namespace MWRender
{
    // One transaction replaces all affected modular parts. A shared full-body
    // skin is sampled only once per fragment, including when both legs are cut.
    struct LimbSeveringGeometry
    {
        using BoneWorld = std::function<std::optional<osg::Matrixf>(std::string_view)>;
        using BoneRegion = std::function<unsigned(std::string_view)>;
        using BoneTargets = std::function<unsigned(std::string_view)>;
        struct Cut
        {
            unsigned bit = 0;
            std::string name, parent, child;
            osg::Matrixf parentWorld, childWorld, childInParent;
            osg::Vec3f center, normal;
            float distance = 0, length = 0;
            std::array<osg::ref_ptr<osg::Group>, 2> caps{ new osg::Group, new osg::Group };
            std::array<osg::Vec3f, 2> surfaceCenters, surfaceU, surfaceV;
            std::array<float, 2> surfaceRadii{};
        };
        struct Source;
        class Surface;
        unsigned mask = 0;
        std::vector<Cut> cuts;
        // Region zero follows the pelvis; subsequent regions follow cut children.
        std::vector<osg::ref_ptr<osg::Group>> regions;
        std::vector<std::string> anchors;
        std::vector<osg::Matrixf> inverse;
        std::vector<std::pair<osg::ref_ptr<osg::Node>, osg::Node::NodeMask>> originals;
        std::vector<std::shared_ptr<Source>> sources;
        std::vector<osg::ref_ptr<Surface>> surfaces;
        std::size_t sourceTriangles = 0, resultTriangles = 0, capTriangles = 0, capLoops = 0;
        std::size_t articulatedSurfaces = 0, decalTriangles = 0, seamFallbacks = 0;
        float reconstructionError = 0;
        double preparationMilliseconds = 0;
        int failure = 0;
        bool committed = false;

        LimbSeveringGeometry();
        ~LimbSeveringGeometry();
        void restore();
        bool build(const std::vector<DecapitationGeometry::Snapshot>& snapshots,
            std::vector<Cut> requestedCuts, const BoneWorld& posedBones, const BoneWorld& liveBones,
            const BoneRegion& boneRegion, const BoneTargets& boneTargets, const BoneTargets& distalTargets,
            osg::StateSet* stumpCap, osg::StateSet* fragmentCap);
        void addDecal(osg::Drawable* source, const SceneUtil::SurfaceDecals::PatchData& patch);
        void clearDecals();
        void prepareDrawBuffers();
    };
}
#endif
