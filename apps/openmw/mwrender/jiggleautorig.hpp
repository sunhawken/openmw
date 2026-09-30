#ifndef OPENMW_MWRENDER_JIGGLEAUTORIG_H
#define OPENMW_MWRENDER_JIGGLEAUTORIG_H

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Vec2f>
#include <osg/Vec3f>

namespace osg
{
    class Group;
}

namespace MWRender
{
    /// In-engine port of the Jiggle Bone Auto-Rigger (autorig.py): procedurally add
    /// breast/butt jiggle bones + proximity weights to a female body at load when the
    /// meshes don't already carry them, so pre-rigging with the .bat tool is optional.
    ///
    /// The caller (NpcAnimation) invokes this after actor body/equipment parts are attached. Female-body
    /// parts have been attached under @p objectRoot - including on every equip/unequip,
    /// so it doubles as a resync: it is idempotent (won't re-create existing bones or
    /// re-paint meshes that already carry them) and paints any newly attached part that
    /// lacks jiggle bones (e.g. armor equipped over the body). It no-ops when the feature
    /// is off, or leaves the actor alone if it was rigged externally by the .bat.
    ///
    /// It detects breast/butt anchors from the mesh geometry (autorig.py's band + protrusion
    /// heuristic), injects a jiggle bone as an identity child of the weighted parent bone
    /// (Spine2 for breast, Pelvis for butt) with a JiggleBoneController, then cone-paints
    /// proximity weights onto the body RigGeometry (reusing the parent's inverse-bind so the
    /// rest pose is exact) and feathers the seams. Gated to female actors, never creatures.
    /// Controlled by the "jiggle auto rig" setting; "jiggle auto rig debug" logs detection.
    namespace JiggleAutoRig
    {
        /// @param isPlayer when true, the actor's body mesh is recorded as the "current player mesh"
        /// and its saved per-mesh Z offset (if any) is pushed into the live breast/butt Z sliders,
        /// so the sliders track whichever body mesh the player is currently using.
        /// @param allowBodyAutoRig false skips female-body breast/butt/thigh generation.
        void run(osg::Group* objectRoot, bool isPlayer = false, bool allowBodyAutoRig = true,
            std::string_view actorName = {});

        /// Geometry of an actor's current outfit (all attached body/clothing/armor meshes) plus the
        /// jiggle anchors, for the Retarget Jiggle window. Positions are mesh space: +x forward,
        /// +y the character's left, +z up.
        struct RetargetPreview
        {
            std::string mChestMesh; // lower-case mesh file keying the saved breast anchors ("" = none)
            std::string mPelvisMesh; // lower-case mesh file keying the saved butt anchors
            std::vector<osg::Vec3f> mVertices; // triangle corners, three per triangle
            std::vector<osg::Vec3f> mNormals; // one per corner
            float mYMin = 0.f, mYMax = 0.f, mZMin = 0.f, mZMax = 0.f;
            /// 0 = breast left, 1 = breast right, 2 = butt left, 3 = butt right; (y, z) in mesh space.
            std::array<std::optional<osg::Vec2f>, 4> mAuto; // where the auto-rigger would put them
            std::array<std::optional<osg::Vec2f>, 4> mSaved; // hand-placed positions saved for these meshes
        };

        /// Fills @p out from @p objectRoot (an actor's object root). Returns false when there is no rigged mesh.
        bool buildRetargetPreview(osg::Group* objectRoot, RetargetPreview& out);
    }
}

#endif
