#ifndef OPENMW_MWRENDER_BISECTION_H
#define OPENMW_MWRENDER_BISECTION_H

#include "decapitation.hpp"
#include <memory>

namespace MWRender
{
    // One actor owns all replacement geometry. The original modular extremities
    // remain on their original bones; only central body-part surfaces are split.
    struct BisectionGeometry
    {
        using BoneWorld = std::function<std::optional<osg::Matrixf>(std::string_view)>;
        // 0 = pelvis branch, 1 = Spine1 branch, -1 = unsupported bone.
        using BoneSide = std::function<int(std::string_view)>;
        // -1 = central vertex; 0/1 protect leg/arm vertices from a waist plane.
        using LimbSide = std::function<int(std::string_view)>;
        struct Source;
        class Surface;
        osg::ref_ptr<osg::Group> lower = new osg::Group;
        osg::ref_ptr<osg::Group> upper = new osg::Group;
        std::vector<std::pair<osg::ref_ptr<osg::Node>, osg::Node::NodeMask>> originals;
        std::vector<std::shared_ptr<Source>> sources;
        std::vector<osg::ref_ptr<Surface>> surfaces;
        std::array<osg::Matrixf, 2> inverse;
        osg::Matrixf spineInPelvis;
        std::array<osg::Vec3f, 2> surfaceCenters, surfaceU, surfaceV;
        osg::Vec3f center, normal;
        float surfaceRadius = 0, cutDistance = 0, reconstructionError = 0;
        std::size_t sourceTriangles = 0, resultTriangles = 0, capTriangles = 0, capLoops = 0;
        std::size_t protectedTriangles = 0, articulatedSurfaces = 0, decalTriangles = 0;
        double preparationMilliseconds = 0;
        int failure = 0;
        bool committed = false;

        BisectionGeometry();
        ~BisectionGeometry();
        void restore();
        bool build(const std::vector<DecapitationGeometry::Snapshot>& snapshots,
            const BoneWorld& bones, const BoneWorld& liveBones, const BoneSide& boneSide, const LimbSide& limbSide,
            const osg::Matrixf& pelvis, const osg::Matrixf& spine, const osg::Vec3f& axis,
            float offset, osg::StateSet* lowerCap, osg::StateSet* upperCap);
        void addDecal(osg::Drawable* source, const SceneUtil::SurfaceDecals::PatchData& patch);
        void clearDecals();
        // Called after shader recreation. Both draw buffers retain its material
        // state and have independent dynamic arrays/VBOs, as RigGeometry does.
        void prepareDrawBuffers();
    };
}
#endif
