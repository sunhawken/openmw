#ifndef OPENMW_MWRENDER_JIGGLEAUTORIG_H
#define OPENMW_MWRENDER_JIGGLEAUTORIG_H

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
    /// The caller (NpcAnimation) invokes this only for female actors, after the body
    /// parts have been attached under @p objectRoot. It no-ops when the feature is off
    /// or when the body already has jiggle bones (rigged externally by the .bat).
    ///
    /// It detects breast/butt anchors from the mesh geometry (autorig.py's band + protrusion
    /// heuristic), injects a jiggle bone as an identity child of the weighted parent bone
    /// (Spine2 for breast, Pelvis for butt) with a JiggleBoneController, then cone-paints
    /// proximity weights onto the body RigGeometry (reusing the parent's inverse-bind so the
    /// rest pose is exact) and feathers the seams. Gated to female actors, never creatures.
    /// Controlled by the "jiggle auto rig" setting; "jiggle auto rig debug" logs detection.
    namespace JiggleAutoRig
    {
        void run(osg::Group* objectRoot);
    }
}

#endif
