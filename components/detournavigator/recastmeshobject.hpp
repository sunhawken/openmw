#ifndef OPENMW_COMPONENTS_DETOURNAVIGATOR_RECASTMESHOBJECT_H
#define OPENMW_COMPONENTS_DETOURNAVIGATOR_RECASTMESHOBJECT_H

#include "areatype.hpp"
#include "objecttransform.hpp"

#include <components/resource/physicsshape.hpp>

#include <Jolt/Physics/Collision/Shape/CompoundShape.h>

#include <osg/Matrixd>
#include <osg/ref_ptr>

#include <functional>
#include <vector>

namespace DetourNavigator
{
    static inline osg::Matrixd getSubShapeTransform(JPH::CompoundShape::SubShape subShape)
    {
        auto subRot = subShape.GetRotation();
        auto comPos = subShape.GetPositionCOM();
        JPH::Vec3 inCenterOfMass = JPH::Vec3(0.0f, 0.0f, 0.0f);
        auto inPosition = comPos + inCenterOfMass - subRot * subShape.mShape->GetCenterOfMass();
        osg::Matrixd trans;
        trans.makeRotate(osg::Quat(subRot.GetX(), subRot.GetY(), subRot.GetZ(), subRot.GetW()));
        trans.setTrans(osg::Vec3f(inPosition.GetX(), inPosition.GetY(), inPosition.GetZ()));
        return trans;
    }

    class CollisionShape
    {
    public:
        CollisionShape(osg::ref_ptr<const Resource::PhysicsShapeInstance> instance, const JPH::Shape& shape,
            const ObjectTransform& transform)
            : mInstance(std::move(instance))
            , mShape(shape)
            , mObjectTransform(transform)
        {
        }

        const osg::ref_ptr<const Resource::PhysicsShapeInstance>& getInstance() const { return mInstance; }
        const JPH::Shape& getShape() const { return mShape; }
        const ObjectTransform& getObjectTransform() const { return mObjectTransform; }

    private:
        osg::ref_ptr<const Resource::PhysicsShapeInstance> mInstance;
        std::reference_wrapper<const JPH::Shape> mShape;
        ObjectTransform mObjectTransform;
    };

    class ChildRecastMeshObject
    {
    public:
        ChildRecastMeshObject(
            const JPH::CompoundShape::SubShape& shape, const osg::Matrixd& transform, const AreaType areaType);

        bool update(const osg::Matrixd& transform, const AreaType areaType);

        const JPH::RefConst<JPH::Shape> getShape() const { return mShape.get().mShape; }

        const osg::Matrixd& getTransform() const { return mTransform; }

        AreaType getAreaType() const { return mAreaType; }

    private:
        std::reference_wrapper<const JPH::CompoundShape::SubShape> mShape;
        osg::Matrixd mTransform;
        AreaType mAreaType;
        JPH::Vec3 mLocalScaling;
        std::vector<ChildRecastMeshObject> mChildren;
    };

    class RecastMeshObject
    {
    public:
        RecastMeshObject(const CollisionShape& shape, const osg::Matrixd& transform, const AreaType areaType);

        bool update(const osg::Matrixd& transform, const AreaType areaType);

        const osg::ref_ptr<const Resource::PhysicsShapeInstance>& getInstance() const { return mInstance; }

        const JPH::RefConst<JPH::Shape> getShape() const { return mShape; }

        const osg::Matrixd& getTransform() const { return mTransform; }

        AreaType getAreaType() const { return mAreaType; }

        const ObjectTransform& getObjectTransform() const { return mObjectTransform; }

    private:
        osg::ref_ptr<const Resource::PhysicsShapeInstance> mInstance;
        ObjectTransform mObjectTransform;

        JPH::RefConst<JPH::Shape> mShape;
        osg::Matrixd mTransform;
        AreaType mAreaType;
        JPH::Vec3 mLocalScaling;
        std::vector<ChildRecastMeshObject> mChildren;
    };
}

#endif
