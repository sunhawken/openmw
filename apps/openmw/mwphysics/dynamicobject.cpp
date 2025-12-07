#include "dynamicobject.hpp"
#include "mtphysics.hpp"
#include "physicssystem.hpp"
#include "joltlayers.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/convert.hpp>
#include <components/physicshelpers/collisionobject.hpp>
#include <components/resource/physicsshape.hpp>

// IMPORTANT: Jolt/Jolt.h must be included first before any other Jolt headers
#include <Jolt/Jolt.h>

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>

namespace MWPhysics
{
    DynamicObject::DynamicObject(const MWWorld::Ptr& ptr, osg::ref_ptr<Resource::PhysicsShapeInstance> shapeInstance,
        osg::Quat rotation, float mass, PhysicsTaskScheduler* scheduler, PhysicsSystem* physicsSystem)
        : PtrHolder(ptr, ptr.getRefData().getPosition().asVec3())
        , mShapeInstance(std::move(shapeInstance))
        , mMass(mass)
        , mScale(ptr.getCellRef().getScale(), ptr.getCellRef().getScale(), ptr.getCellRef().getScale())
        , mTaskScheduler(scheduler)
        , mPhysicsSystem(physicsSystem)
    {
        mBasePhysicsShape = mShapeInstance->mCollisionShape.GetPtr();
        mUsesScaledShape = !(mScale.x() == 1.0f && mScale.y() == 1.0f && mScale.z() == 1.0f);

        JPH::Shape* finalShape = mUsesScaledShape
            ? new JPH::ScaledShape(mBasePhysicsShape.GetPtr(), Misc::Convert::toJolt<JPH::Vec3>(mScale))
            : mBasePhysicsShape.GetPtr();

        // Create as Dynamic body in DYNAMIC_WORLD layer
        JPH::BodyCreationSettings bodyCreationSettings = PhysicsSystemHelpers::makePhysicsBodySettings(
            finalShape, mPosition, rotation, Layers::DYNAMIC_WORLD, JPH::EMotionType::Dynamic);

        // Configure physics properties for realistic behavior
        bodyCreationSettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bodyCreationSettings.mMassPropertiesOverride.mMass = mass;

        // Enable gravity (default gravity factor is 1.0)
        bodyCreationSettings.mGravityFactor = 1.0f;

        // Set reasonable damping to prevent infinite rolling/sliding
        bodyCreationSettings.mLinearDamping = 0.1f;
        bodyCreationSettings.mAngularDamping = 0.2f;

        // Set friction and restitution for realistic collisions
        bodyCreationSettings.mFriction = 0.5f;
        bodyCreationSettings.mRestitution = 0.3f;

        // Use discrete motion quality (LinearCast is for fast-moving objects like projectiles)
        bodyCreationSettings.mMotionQuality = JPH::EMotionQuality::Discrete;

        // Allow sleeping when at rest to save performance
        bodyCreationSettings.mAllowSleeping = true;

        mPhysicsBody = mTaskScheduler->createPhysicsBody(bodyCreationSettings);
        if (mPhysicsBody != nullptr)
        {
            mPhysicsBody->SetUserData(reinterpret_cast<uintptr_t>(this));

            // Add to world and activate it
            mTaskScheduler->addCollisionObject(mPhysicsBody, true);

            Log(Debug::Verbose) << "Created DynamicObject for " << ptr.getCellRef().getRefId()
                               << " with mass " << mass;
        }
    }

    DynamicObject::~DynamicObject()
    {
        if (mPhysicsBody != nullptr)
        {
            mTaskScheduler->removeCollisionObject(mPhysicsBody);
            mTaskScheduler->destroyCollisionObject(mPhysicsBody);
        }
    }

    const Resource::PhysicsShapeInstance* DynamicObject::getShapeInstance() const
    {
        return mShapeInstance.get();
    }

    void DynamicObject::setScale(float scale)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        osg::Vec3f newScale = { scale, scale, scale };
        if (mScale != newScale)
        {
            mScale = newScale;

            // Update the shape
            JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
            JPH::ScaledShape* newShape;
            JPH::ShapeRefC shapeRef = mPhysicsBody->GetShape();

            if (!mUsesScaledShape)
            {
                newShape = new JPH::ScaledShape(shapeRef.GetPtr(), Misc::Convert::toJolt<JPH::Vec3>(mScale));
            }
            else
            {
                const JPH::ScaledShape* bodyShape = static_cast<const JPH::ScaledShape*>(shapeRef.GetPtr());
                const JPH::Shape* innerShape = bodyShape->GetInnerShape();
                newShape = new JPH::ScaledShape(innerShape, Misc::Convert::toJolt<JPH::Vec3>(mScale));
            }

            mUsesScaledShape = true;
            bodyInterface.SetShape(getPhysicsBody(), newShape, true, JPH::EActivation::Activate);
        }
    }

    osg::Vec3f DynamicObject::getSimulationPosition() const
    {
        JPH::BodyLockRead lock(mTaskScheduler->getBodyLockInterface(), getPhysicsBody());
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            return Misc::Convert::toOsg(body.GetCenterOfMassPosition());
        }
        return mPosition;
    }

    osg::Quat DynamicObject::getSimulationRotation() const
    {
        JPH::BodyLockRead lock(mTaskScheduler->getBodyLockInterface(), getPhysicsBody());
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            return Misc::Convert::toOsg(body.GetRotation());
        }
        return osg::Quat();
    }

    void DynamicObject::applyImpulse(const osg::Vec3f& impulse)
    {
        JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
        bodyInterface.AddImpulse(getPhysicsBody(), Misc::Convert::toJolt<JPH::Vec3>(impulse));
    }

    void DynamicObject::applyForce(const osg::Vec3f& force)
    {
        JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
        bodyInterface.AddForce(getPhysicsBody(), Misc::Convert::toJolt<JPH::Vec3>(force));
    }

    void DynamicObject::setLinearVelocity(const osg::Vec3f& velocity)
    {
        JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
        bodyInterface.SetLinearVelocity(getPhysicsBody(), Misc::Convert::toJolt<JPH::Vec3>(velocity));
    }

    osg::Vec3f DynamicObject::getLinearVelocity() const
    {
        JPH::BodyLockRead lock(mTaskScheduler->getBodyLockInterface(), getPhysicsBody());
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            return Misc::Convert::toOsg(body.GetLinearVelocity());
        }
        return osg::Vec3f();
    }

    bool DynamicObject::isActive() const
    {
        JPH::BodyLockRead lock(mTaskScheduler->getBodyLockInterface(), getPhysicsBody());
        if (lock.Succeeded())
        {
            return lock.GetBody().IsActive();
        }
        return false;
    }

    void DynamicObject::activate()
    {
        JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
        bodyInterface.ActivateBody(getPhysicsBody());
    }

    bool DynamicObject::onContactValidate(const JPH::Body& withBody)
    {
        // Allow all collisions for dynamic objects
        return true;
    }

    void DynamicObject::onContactAdded(const JPH::Body& withBody, const JPH::ContactManifold& inManifold,
        JPH::ContactSettings& ioSettings)
    {
        // Could add sound effects, particle effects, etc. here based on collision
        // For now, just let Jolt handle the physics response
    }
}
