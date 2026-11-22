#ifndef OPENMW_MWWORLD_RAGDOLLDROPPEDITEM_H
#define OPENMW_MWWORLD_RAGDOLLDROPPEDITEM_H

#include "ptr.hpp"
#include <osg/Vec3f>

namespace MWWorld
{
    // Bounded runtime diagnostics only. The item itself is an ordinary saved,
    // lootable cell reference; SafePtr follows its normal cell/pickup lifetime.
    struct RagdollDroppedItem
    {
        SafePtr mObject;
        ESM::RefId mSourceItemId;
        int mSlot = -1;
        osg::Vec3f mInitialLinearVelocity, mInitialAngularVelocity;
        float mMaxWorldVertexError = 0.f;
        float mSourceCondition = -1.f, mInitialCondition = -1.f;
        float mSourceEnchantmentCharge = -1.f, mInitialEnchantmentCharge = -1.f;
        bool mItemDataPreserved = false;
        ESM::RefNum mSourceObjectId;
    };
}

#endif
