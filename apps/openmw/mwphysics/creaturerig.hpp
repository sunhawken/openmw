#ifndef OPENMW_MWPHYSICS_CREATURERIG_H
#define OPENMW_MWPHYSICS_CREATURERIG_H
#include <string>
#include <vector>
#include <osg/Vec3f>
namespace MWWorld
{
    class Ptr;
}
namespace MWPhysics
{
    // One rigid part of a creature ragdoll. Its capsule is stored in the bone's own
    // space, so it fits whatever pose the creature is in when it falls.
    struct CreatureRigPart
    {
        std::string mBone;
        int mParent = -1; // earlier index in the rig; the first part is the root
        osg::Vec3f mCenter, mAxis;
        float mHalfLength = 0.f, mRadius = 0.f, mMass = 0.f;
        float mSwing1 = 0.f, mSwing2 = 0.f, mTwist = 0.f;
    };

    struct CreatureRig
    {
        std::vector<CreatureRigPart> mParts;
        float mTotalMass = 0.f;
        // A heavy body answers a kick more slowly than a light one.
        // The actor's own scale makes it heavier too.
        float kickScale(float actorScale = 1.f) const;
    };

    // The rig planned for this creature's model by scripts/creatures/build_rigs.py, or null
    // for NPCs, the player, set pieces and models with no rig: those keep their usual behaviour.
    const CreatureRig* findCreatureRig(const MWWorld::Ptr& ptr);
}
#endif
