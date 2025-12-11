#ifndef OPENMW_MWPHYSICS_HEIGHTFIELD_H
#define OPENMW_MWPHYSICS_HEIGHTFIELD_H

#include <osg/ref_ptr>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <memory>
#include <vector>

namespace osg
{
    class Object;
}

namespace MWPhysics
{
    class PhysicsTaskScheduler;

    class HeightField
    {
    public:
        HeightField(const float* heights, int x, int y, int size, int verts, float minH, float maxH,
            const osg::Object* holdObject, PhysicsTaskScheduler* scheduler);
        ~HeightField();

        const osg::Vec3f& getOrigin() const { return mWorldOrigin; }

        JPH::BodyID getPhysicsBody() const
        {
            if (mPhysicsBody == nullptr)
                return JPH::BodyID();
            return mPhysicsBody->GetID();
        }

        // Mark body as already removed (used by batch removal to prevent double-free)
        void markBodyRemoved() { mPhysicsBody = nullptr; }

    private:
        JPH::Body* mPhysicsBody = nullptr; // NOTE: memory is managed by Jolt!
        JPH::ShapeRefC mShapeReference;
        osg::Vec3f mWorldOrigin;
        osg::ref_ptr<const osg::Object> mHoldObject;

        PhysicsTaskScheduler* mTaskScheduler;

        void operator=(const HeightField&);
        HeightField(const HeightField&);
    };
}

#endif
