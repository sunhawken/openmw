#ifndef OPENMW_MWWATER_OBJECT_H
#define OPENMW_MWWATER_OBJECT_H

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>

namespace MWPhysics
{
    class PhysicsTaskScheduler;
    class MWWater
    {
        public:
            MWWater(PhysicsTaskScheduler* scheduler, float height);
            ~MWWater();

        private:
            JPH::Body* mPhysicsBody;
            PhysicsTaskScheduler* mTaskScheduler;
    };
}

#endif
