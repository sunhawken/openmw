#ifndef OPENMW_MWRENDER_SEAMWELDER_H
#define OPENMW_MWRENDER_SEAMWELDER_H

#include <cstddef>
#include <vector>

#include <osg/ref_ptr>

#include <components/sceneutil/riggeometry.hpp>

namespace osg
{
    class Group;
}

namespace MWRender
{
    /// Auto seam welder for jiggle physics.
    ///
    /// Jiggle bones move the vertices of the meshes skinned to them (a body
    /// replacer's breast/butt), but a rigid mesh worn over that body (armor or
    /// clothing that has no jiggle bones) stays put, so a visible crack opens
    /// along the seam where the two meshes met at rest pose.
    ///
    /// This class finds, for a character, which vertices of the rigid meshes were
    /// coincident (at rest/bind pose) with vertices of the jiggle-driven meshes,
    /// and records those pairs. Later stages copy the jiggle mesh's live deformed
    /// position onto the rigid mesh's welded vertices each frame to close the crack.
    ///
    /// STAGE 1: build + classify + correspondence only. Nothing is welded yet; the
    /// map is logged (with "jiggle seam weld debug") so the detection can be
    /// verified in-game before the renderer is touched.
    class SeamWelder
    {
    public:
        /// One rigid-mesh vertex welded onto one jiggle-mesh vertex.
        struct WeldPair
        {
            SceneUtil::RigGeometry* mRigid; // mesh whose vertex gets snapped
            std::size_t mRigidVertex;
            SceneUtil::RigGeometry* mJiggle; // mesh whose live position is copied
            std::size_t mJiggleVertex;
        };

        /// (Re)scan the character's scene graph, classify meshes as jiggle-driven
        /// vs. rigid, and build the rest-pose weld map. Safe to call repeatedly
        /// (e.g. on equipment change). No effect when the feature is disabled.
        void build(osg::Group* objectRoot);

        void clear();

        bool empty() const { return mPairs.empty(); }
        const std::vector<WeldPair>& pairs() const { return mPairs; }

    private:
        std::vector<WeldPair> mPairs;
        // Keep the geometries referenced so the raw pointers in mPairs stay valid.
        std::vector<osg::ref_ptr<SceneUtil::RigGeometry>> mReferenced;
    };
}

#endif
