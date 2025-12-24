#ifndef OPENMW_MWPHYSICS_DYNAMICOBJECT_H
#define OPENMW_MWPHYSICS_DYNAMICOBJECT_H

#include "collisionshapeconfig.hpp"
#include "ptrholder.hpp"

#include <osg/Node>

#include <mutex>

namespace Resource
{
    class PhysicsShapeInstance;
}

namespace MWPhysics
{
    class PhysicsTaskScheduler;
    class PhysicsSystem;

    // A dynamic physics object that responds to gravity and collisions
    // Used for Oblivion-style item physics (items that can be pushed around)
    class DynamicObject final : public PtrHolder
    {
    public:
        DynamicObject(const MWWorld::Ptr& ptr, osg::ref_ptr<Resource::PhysicsShapeInstance> shapeInstance,
            osg::Quat rotation, float mass, PhysicsTaskScheduler* scheduler, PhysicsSystem* physicsSystem,
            DynamicShapeType shapeType = DynamicShapeType::Box);
        ~DynamicObject() override;

        const Resource::PhysicsShapeInstance* getShapeInstance() const;

        void setScale(float scale);

        // Get the current simulation position (from Jolt)
        osg::Vec3f getSimulationPosition() const override;

        // Get the current simulation rotation (from Jolt)
        osg::Quat getSimulationRotation() const;

        // Apply an impulse to the object (for pushing/hitting)
        void applyImpulse(const osg::Vec3f& impulse);

        // Apply a force to the object (continuous push)
        void applyForce(const osg::Vec3f& force);

        // Set linear velocity directly
        void setLinearVelocity(const osg::Vec3f& velocity);

        // Get linear velocity
        osg::Vec3f getLinearVelocity() const;

        // Set angular velocity directly
        void setAngularVelocity(const osg::Vec3f& velocity);

        // Check if the object is currently active (moving)
        bool isActive() const;

        // Wake up the object (make it active for simulation)
        void activate();

        // Contact callbacks
        void onContactAdded(const JPH::Body& withBody, const JPH::ContactManifold& inManifold,
            JPH::ContactSettings& ioSettings) override;
        bool onContactValidate(const JPH::Body& withBody) override;

        // Get mass
        float getMass() const { return mMass; }

        // Buoyancy support
        void updateBuoyancy(float waterHeight, float gravity, float dt);
        bool isInWater() const { return mInWater; }
        float getSubmersionDepth() const { return mSubmersionDepth; }

        // Water zone tracking - indicates object is near enough to water to need buoyancy checks
        bool isInWaterZone() const { return mInWaterZone; }
        void setInWaterZone(bool inZone) { mInWaterZone = inZone; }

    private:
        osg::ref_ptr<Resource::PhysicsShapeInstance> mShapeInstance;
        JPH::Ref<JPH::Shape> mBasePhysicsShape;
        float mMass;
        osg::Vec3f mScale;
        bool mUsesScaledShape = false;
        mutable std::mutex mMutex;
        PhysicsTaskScheduler* mTaskScheduler;
        PhysicsSystem* mPhysicsSystem;

        // Buoyancy state
        bool mInWater = false;
        bool mInWaterZone = false;  // Near water, needs buoyancy checks
        float mSubmersionDepth = 0.0f;
    };
}

#endif
