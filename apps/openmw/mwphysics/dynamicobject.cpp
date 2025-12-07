#include "dynamicobject.hpp"
#include "mtphysics.hpp"
#include "physicssystem.hpp"
#include "joltlayers.hpp"

#include <algorithm>

#include <components/debug/debuglog.hpp>
#include <components/misc/convert.hpp>
#include <components/physicshelpers/collisionobject.hpp>
#include <components/resource/physicsshape.hpp>

// IMPORTANT: Jolt/Jolt.h must be included first before any other Jolt headers
#include <Jolt/Jolt.h>

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>

namespace MWPhysics
{
    // Helper function to create a convex shape from a mesh shape for dynamic objects
    // Jolt's MeshShape cannot collide with other MeshShapes or HeightfieldShapes,
    // so we need to use a convex shape for dynamic objects.
    static JPH::Ref<JPH::Shape> createConvexShapeFromMesh(const JPH::Shape* meshShape)
    {
        // Get the local bounds of the mesh shape
        JPH::AABox bounds = meshShape->GetLocalBounds();
        JPH::Vec3 halfExtents = bounds.GetExtent();
        JPH::Vec3 center = bounds.GetCenter();

        // Ensure minimum size to avoid degenerate shapes
        const float minSize = 1.0f;
        halfExtents = JPH::Vec3::sMax(halfExtents, JPH::Vec3::sReplicate(minSize));

        // Create a box shape with the same bounds
        // Using a small convex radius for better collision detection
        JPH::BoxShapeSettings boxSettings(halfExtents, 0.05f);
        auto result = boxSettings.Create();

        if (result.HasError())
        {
            Log(Debug::Warning) << "Failed to create box shape for dynamic object: " << result.GetError();
            return nullptr;
        }

        // If the mesh center is not at origin, we need to offset the shape
        // For now, we assume the shape is centered (most items are)
        // A more robust solution would use OffsetCenterOfMassShape
        return result.Get();
    }

    DynamicObject::DynamicObject(const MWWorld::Ptr& ptr, osg::ref_ptr<Resource::PhysicsShapeInstance> shapeInstance,
        osg::Quat rotation, float mass, PhysicsTaskScheduler* scheduler, PhysicsSystem* physicsSystem)
        : PtrHolder(ptr, ptr.getRefData().getPosition().asVec3())
        , mShapeInstance(std::move(shapeInstance))
        , mMass(mass)
        , mScale(ptr.getCellRef().getScale(), ptr.getCellRef().getScale(), ptr.getCellRef().getScale())
        , mTaskScheduler(scheduler)
        , mPhysicsSystem(physicsSystem)
    {
        // For dynamic objects, we need to use a convex shape instead of a mesh shape.
        // Jolt's MeshShape cannot collide with other MeshShapes or HeightfieldShapes,
        // which means dynamic objects using mesh shapes would pass through terrain and walls.
        JPH::Ref<JPH::Shape> convexShape = createConvexShapeFromMesh(mShapeInstance->mCollisionShape.GetPtr());
        if (!convexShape)
        {
            Log(Debug::Error) << "Failed to create convex shape for dynamic object: " << ptr.getCellRef().getRefId();
            return;
        }

        mBasePhysicsShape = convexShape;
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

        // Use LinearCast motion quality for continuous collision detection
        // This prevents objects from tunneling through walls, floors, and terrain
        bodyCreationSettings.mMotionQuality = JPH::EMotionQuality::LinearCast;

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
        if (mPhysicsBody == nullptr)
            return;

        JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
        bodyInterface.AddImpulse(getPhysicsBody(), Misc::Convert::toJolt<JPH::Vec3>(impulse));
    }

    void DynamicObject::applyForce(const osg::Vec3f& force)
    {
        if (mPhysicsBody == nullptr)
            return;

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

    void DynamicObject::updateBuoyancy(float waterHeight, float gravity, float dt)
    {
        if (!mBasePhysicsShape)
            return;

        // Get current position
        osg::Vec3f position = getSimulationPosition();

        // Get actual object bounds from the physics shape
        // Note: GetExtent() returns half-extents, and for OpenMW (Z-up) we need the Z component
        JPH::AABox bounds = mBasePhysicsShape->GetLocalBounds();
        // Use the maximum of X and Z extents as the "radius" for buoyancy calculation
        // since objects may be rotated and we want reasonable behavior
        float halfHeight = std::max(bounds.GetExtent().GetZ(), bounds.GetExtent().GetY()) * mScale.z();
        halfHeight = std::max(halfHeight, 5.0f);  // Minimum half-height to ensure some buoyancy interaction

        float objectBottom = position.z() - halfHeight;
        float objectTop = position.z() + halfHeight;

        // Check if object is in water
        if (objectBottom >= waterHeight)
        {
            // Object is above water
            if (mInWater)
            {
                // Just left water
                Log(Debug::Verbose) << "DynamicObject " << getPtr().getCellRef().getRefId()
                                    << " left water at z=" << position.z() << " waterHeight=" << waterHeight;
            }
            mInWater = false;
            mSubmersionDepth = 0.0f;
            return;
        }

        if (!mInWater)
        {
            // Just entered water
            Log(Debug::Info) << "DynamicObject " << getPtr().getCellRef().getRefId()
                             << " entered water at z=" << position.z() << " waterHeight=" << waterHeight;
        }
        mInWater = true;

        // Calculate submersion depth (how deep the object center is below water)
        mSubmersionDepth = waterHeight - position.z();

        // Calculate submersion factor (0 to 1, clamped)
        float submersionFactor;
        if (objectTop <= waterHeight)
        {
            // Fully submerged
            submersionFactor = 1.0f;
        }
        else
        {
            // Partially submerged
            float objectHeight = objectTop - objectBottom;
            if (objectHeight > 0.0f)
                submersionFactor = (waterHeight - objectBottom) / objectHeight;
            else
                submersionFactor = 1.0f;
        }
        submersionFactor = std::clamp(submersionFactor, 0.0f, 1.0f);

        // IMPORTANT: Activate the body FIRST when in water so it doesn't sleep
        // Forces applied to sleeping bodies are ignored in Jolt!
        activate();

        // Calculate how far below water surface the object is
        float depthBelowSurface = waterHeight - position.z();

        // Buoyancy: objects should float up to the water surface
        // Use a spring-like force that increases with depth
        // This creates stable floating behavior at the surface
        constexpr float buoyancyStrength = 5.0f;  // Spring constant
        constexpr float dampingFactor = 2.0f;     // Damping to prevent oscillation

        // Buoyancy force: stronger the deeper you are
        float buoyancyForce = buoyancyStrength * mMass * gravity * submersionFactor;

        // Add extra force based on depth to help objects rise from bottom
        if (depthBelowSurface > 0)
        {
            // Exponential increase for deeply submerged objects
            buoyancyForce += mMass * gravity * std::min(depthBelowSurface / 10.0f, 5.0f);
        }

        // Apply upward buoyancy force
        applyForce(osg::Vec3f(0.0f, 0.0f, buoyancyForce));

        // Apply water drag (resistance) to slow down movement in water
        osg::Vec3f velocity = getLinearVelocity();
        float speed = velocity.length();

        // Strong vertical damping to stabilize floating
        if (std::abs(velocity.z()) > 0.1f)
        {
            float verticalDamping = -velocity.z() * dampingFactor * mMass;
            applyForce(osg::Vec3f(0.0f, 0.0f, verticalDamping));
        }

        if (speed > 0.01f)
        {
            // Drag force opposes motion, proportional to velocity squared
            constexpr float dragCoefficient = 1.5f;  // Increased drag in water
            float dragMagnitude = dragCoefficient * submersionFactor * speed * speed;

            // Limit drag to prevent instability
            dragMagnitude = std::min(dragMagnitude, speed * mMass / dt);

            osg::Vec3f dragForce = -velocity * (dragMagnitude / speed);
            applyForce(dragForce);
        }

        // Debug logging every ~1 second (assuming 60fps, log every 60 frames)
        static int frameCounter = 0;
        if (++frameCounter % 60 == 0)
        {
            Log(Debug::Info) << "Buoyancy: " << getPtr().getCellRef().getRefId()
                             << " z=" << position.z() << " water=" << waterHeight
                             << " depth=" << depthBelowSurface
                             << " force=" << buoyancyForce << " vel.z=" << velocity.z();
        }
    }
}
