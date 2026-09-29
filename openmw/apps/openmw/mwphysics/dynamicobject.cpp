#include "dynamicobject.hpp"
#include "collisionshapeconfig.hpp"
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
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

namespace MWPhysics
{
    // Helper function to create a convex shape from a mesh shape for dynamic objects
    // Jolt's MeshShape cannot collide with other MeshShapes or HeightfieldShapes,
    // so we need to use a convex shape for dynamic objects.
    // The shapeType parameter determines what kind of convex shape to create.
    static JPH::Ref<JPH::Shape> createConvexShapeFromMesh(const JPH::Shape* meshShape, DynamicShapeType shapeType)
    {
        // Get the local bounds of the mesh shape
        JPH::AABox bounds = meshShape->GetLocalBounds();
        JPH::Vec3 halfExtents = bounds.GetExtent();

        // Ensure minimum size to avoid degenerate shapes
        const float minSize = 1.0f;
        halfExtents = JPH::Vec3::sMax(halfExtents, JPH::Vec3::sReplicate(minSize));

        JPH::ShapeSettings::ShapeResult result;
        const float convexRadius = 0.05f;

        switch (shapeType)
        {
            case DynamicShapeType::Sphere:
            {
                // Use the maximum extent as radius for a sphere that contains the object
                float radius = std::max({ halfExtents.GetX(), halfExtents.GetY(), halfExtents.GetZ() });
                JPH::SphereShapeSettings sphereSettings(radius);
                result = sphereSettings.Create();
                break;
            }
            case DynamicShapeType::Capsule:
            {
                // Capsule: use average of X/Y as radius, Z as half-height
                float radius = (halfExtents.GetX() + halfExtents.GetY()) * 0.5f;
                float halfHeight = halfExtents.GetZ();
                // Capsule half-height is the cylinder part only, not including the caps
                float cylinderHalfHeight = std::max(halfHeight - radius, 0.0f);
                JPH::CapsuleShape* capsule = new JPH::CapsuleShape(cylinderHalfHeight, radius);
                // Jolt creates capsules along Y-axis by default, rotate 90 degrees around X to align with Z (vertical in OpenMW)
                JPH::Quat shapeRotation = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(90.0f));
                return new JPH::RotatedTranslatedShape(JPH::Vec3::sZero(), shapeRotation, capsule);
            }
            case DynamicShapeType::Cylinder:
            {
                // Cylinder: use average of X/Y as radius, Z as half-height
                float radius = (halfExtents.GetX() + halfExtents.GetY()) * 0.5f;
                float halfHeight = halfExtents.GetZ();
                JPH::CylinderShape* cylinder = new JPH::CylinderShape(halfHeight, radius, convexRadius);
                // Jolt creates cylinders along Y-axis by default, rotate 90 degrees around X to align with Z (vertical in OpenMW)
                JPH::Quat shapeRotation = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(90.0f));
                return new JPH::RotatedTranslatedShape(JPH::Vec3::sZero(), shapeRotation, cylinder);
            }
            case DynamicShapeType::Box:
            default:
            {
                JPH::BoxShapeSettings boxSettings(halfExtents, convexRadius);
                result = boxSettings.Create();
                break;
            }
        }

        if (result.HasError())
        {
            Log(Debug::Warning) << "Failed to create shape for dynamic object: " << result.GetError();
            return nullptr;
        }

        return result.Get();
    }

    DynamicObject::DynamicObject(const MWWorld::Ptr& ptr, osg::ref_ptr<Resource::PhysicsShapeInstance> shapeInstance,
        osg::Quat rotation, float mass, PhysicsTaskScheduler* scheduler, PhysicsSystem* physicsSystem,
        DynamicShapeType shapeType)
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
        JPH::Ref<JPH::Shape> convexShape = createConvexShapeFromMesh(mShapeInstance->mCollisionShape.GetPtr(), shapeType);
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
            // Clear UserData before destroying to prevent dangling pointer access
            mPhysicsBody->SetUserData(0);
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

        // Body may have been removed during cell unload
        if (mPhysicsBody == nullptr)
            return;

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
        if (mPhysicsBody == nullptr)
            return mPosition;
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
        if (mPhysicsBody == nullptr)
            return osg::Quat();
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
        if (mPhysicsBody == nullptr)
            return;
        JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
        bodyInterface.SetLinearVelocity(getPhysicsBody(), Misc::Convert::toJolt<JPH::Vec3>(velocity));
    }

    osg::Vec3f DynamicObject::getLinearVelocity() const
    {
        if (mPhysicsBody == nullptr)
            return osg::Vec3f();
        JPH::BodyLockRead lock(mTaskScheduler->getBodyLockInterface(), getPhysicsBody());
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            return Misc::Convert::toOsg(body.GetLinearVelocity());
        }
        return osg::Vec3f();
    }

    void DynamicObject::setAngularVelocity(const osg::Vec3f& velocity)
    {
        if (mPhysicsBody == nullptr)
            return;
        JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
        bodyInterface.SetAngularVelocity(getPhysicsBody(), Misc::Convert::toJolt<JPH::Vec3>(velocity));
    }

    bool DynamicObject::isActive() const
    {
        if (mPhysicsBody == nullptr)
            return false;
        JPH::BodyLockRead lock(mTaskScheduler->getBodyLockInterface(), getPhysicsBody());
        if (lock.Succeeded())
        {
            return lock.GetBody().IsActive();
        }
        return false;
    }

    void DynamicObject::activate()
    {
        if (mPhysicsBody == nullptr)
            return;
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
        if (mPhysicsBody == nullptr)
            return;

        // Use Jolt's built-in buoyancy system which correctly handles:
        // - World-space shape bounds (works with rotated shapes)
        // - Proper submerged volume calculation
        // - Physically correct buoyancy and drag forces

        // Water surface is a horizontal plane at waterHeight
        // In OpenMW/Jolt coordinate system, Z is up
        JPH::RVec3 surfacePosition(0.0, 0.0, static_cast<double>(waterHeight));
        JPH::Vec3 surfaceNormal(0.0f, 0.0f, 1.0f);  // Points up (out of water)

        // Gravity vector points down in Z
        JPH::Vec3 gravityVec(0.0f, 0.0f, -gravity);

        // Buoyancy parameters (from Jolt documentation):
        // - buoyancy: 1.0 = neutral, < 1.0 sinks, > 1.0 floats
        // - linearDrag: ~0.3-0.5 for water resistance
        // - angularDrag: ~0.01-0.05 for rotational damping
        constexpr float buoyancy = 1.2f;      // Slightly buoyant (most objects float)
        constexpr float linearDrag = 0.5f;    // Water resistance
        constexpr float angularDrag = 0.05f;  // Rotational damping
        JPH::Vec3 fluidVelocity = JPH::Vec3::sZero();  // Still water

        // Use BodyInterface::ApplyBuoyancyImpulse which:
        // 1. Wakes up sleeping bodies automatically
        // 2. Calculates submerged volume using world-space shape geometry
        // 3. Applies proper buoyancy force at center of buoyancy
        // 4. Applies linear and angular drag
        JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
        bool wasInWater = bodyInterface.ApplyBuoyancyImpulse(
            getPhysicsBody(),
            surfacePosition,
            surfaceNormal,
            buoyancy,
            linearDrag,
            angularDrag,
            fluidVelocity,
            gravityVec,
            dt
        );

        // Track water state for effects/sounds
        if (wasInWater != mInWater)
        {
            if (wasInWater)
            {
                Log(Debug::Verbose) << "DynamicObject " << getPtr().getCellRef().getRefId()
                                    << " entered water at waterHeight=" << waterHeight;
            }
            else
            {
                Log(Debug::Verbose) << "DynamicObject " << getPtr().getCellRef().getRefId()
                                    << " left water";
            }
        }
        mInWater = wasInWater;

        // Update submersion depth for gameplay purposes (if needed)
        if (mInWater)
        {
            osg::Vec3f position = getSimulationPosition();
            mSubmersionDepth = waterHeight - position.z();
        }
        else
        {
            mSubmersionDepth = 0.0f;
        }
    }
}
