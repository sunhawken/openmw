#ifndef OPENMW_MWPHYSICS_JOLTCALLBACKS_H
#define OPENMW_MWPHYSICS_JOLTCALLBACKS_H

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace MWPhysics
{
    class ClosestConvexResultCallback : public JPH::CastShapeCollector
    {
    public:
        JPH::BodyID mHitCollisionObject;
        float mClosestHitFraction = 1.0f;
        JPH::Vec3 mHitNormalWorld = JPH::Vec3::sZero();
        JPH::RVec3 mHitPointWorld = JPH::RVec3::sZero();

        explicit ClosestConvexResultCallback(JPH::RVec3 origin)
            : mOrigin(origin)
        {
        }

        virtual void AddHit(const JPH::ShapeCastResult& inResult) override
        {
            // Test if this collision is closer/deeper than the previous one
            float early_out = inResult.GetEarlyOutFraction();
            if (early_out < GetEarlyOutFraction())
            {
                // Update early out fraction to this hit
                UpdateEarlyOutFraction(early_out);

                // Get the contact properties
                mHitCollisionObject = inResult.mBodyID2;
                mHitPointWorld = mOrigin + inResult.mContactPointOn2;
                mHitNormalWorld = -inResult.mPenetrationAxis.Normalized();
                mClosestHitFraction = inResult.mFraction;
            }
        }

        bool hasHit() const { return !mHitCollisionObject.IsInvalid(); }

    protected:
        JPH::RVec3 mOrigin;
    };

    class ContactTestResultCallback : public JPH::CollideShapeCollector
    {
    public:
        explicit ContactTestResultCallback(const JPH::PhysicsSystem* physicsSystem, JPH::BodyID& me, JPH::RVec3 origin)
            : mPhysicsSystem(physicsSystem)
            , mOrigin(origin)
            , mMe(me)
        {
        }

        virtual void AddHit(const JPH::CollideShapeResult& inResult) override
        {
            // If self, skip
            if (inResult.mBodyID2 == mMe)
                return;

            JPH::BodyLockRead lock(mPhysicsSystem->GetBodyLockInterfaceNoLock(), inResult.mBodyID2);
            if (!lock.Succeeded())
                return;

            const JPH::Body& body = lock.GetBody();

            // Skip sensors, unlikely to happen but just incase
            if (body.IsSensor())
                return;

            // If object or actor (PtrHolder) then store hit as a result
            // Check UserData is non-zero before converting (it's set to 0 when object is being destroyed)
            uint64_t userData = body.GetUserData();
            if (userData != 0)
            {
                PtrHolder* holder = Misc::Convert::toPointerFromUserData<PtrHolder>(userData);
                if (holder)
                    mResult.emplace_back(ContactPoint{ holder->getPtr(), Misc::Convert::toOsg(inResult.mContactPointOn2),
                        Misc::Convert::toOsg(inResult.mPenetrationAxis.Normalized()) });
            }

            // NOTE: unlike other jolt collectors, dont early out here as we want ALL HITS
        }

        std::vector<ContactPoint> mResult;

    protected:
        const JPH::PhysicsSystem* mPhysicsSystem;
        JPH::RVec3 mOrigin;
        JPH::BodyID mMe;
    };
}

#endif
