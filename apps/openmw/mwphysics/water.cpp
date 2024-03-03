#include "water.hpp"
#include "mtphysics.hpp"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNING_PUSH
JPH_GCC_SUPPRESS_WARNING("-Wpedantic")
JPH_MSVC_SUPPRESS_WARNING(4201)

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>

JPH_SUPPRESS_WARNING_POP

#include <components/misc/convert.hpp>
#include <components/physicshelpers/collisionobject.hpp>

namespace MWPhysics
{
    MWWater::MWWater(PhysicsTaskScheduler* scheduler, float height)
        : mTaskScheduler(scheduler)
    {
        // TODO: update for jolt, need a custom shape
        JPH::BodyCreationSettings bodyCreationSettings
            = PhysicsSystemHelpers::makePhysicsBodySettings(new JPH::BoxShape(JPH::Vec3(1000000.0f, 1000000.0f, 32.0f)),
                osg::Vec3f(0.0f, 0.0f, height), osg::Quat(1.0f, 0.0f, 0.0f, 0.0f), Layers::WATER);

        mPhysicsBody = mTaskScheduler->createPhysicsBody(bodyCreationSettings);
        mTaskScheduler->addCollisionObject(mPhysicsBody, false);
    }

    MWWater::~MWWater()
    {
        mTaskScheduler->removeCollisionObject(mPhysicsBody);
        mTaskScheduler->destroyCollisionObject(mPhysicsBody);
    }
}
