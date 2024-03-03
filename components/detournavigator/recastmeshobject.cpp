#include "recastmeshobject.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <components/debug/debuglog.hpp>
#include <components/misc/convert.hpp>

#include <cassert>

namespace DetourNavigator
{
    namespace
    {
        bool updateCompoundObject(
            const JPH::CompoundShape& shape, const AreaType areaType, std::vector<ChildRecastMeshObject>& children)
        {
            assert(static_cast<std::size_t>(shape.GetNumSubShapes()) == children.size());
            bool result = false;
            for (int i = 0, num = shape.GetNumSubShapes(); i < num; ++i)
            {
                assert(shape.GetSubShape(i).mShape
                    == std::addressof(*children[static_cast<std::size_t>(i)].getShape().GetPtr()));
                auto shapeTransform = getSubShapeTransform(shape.GetSubShape(i));
                result = children[static_cast<std::size_t>(i)].update(shapeTransform, areaType) || result;
            }
            return result;
        }

        std::vector<ChildRecastMeshObject> makeChildrenObjects(const JPH::CompoundShape& shape, const AreaType areaType)
        {
            std::vector<ChildRecastMeshObject> result;
            for (int i = 0, num = shape.GetNumSubShapes(); i < num; ++i)
            {
                auto shapeTransform = getSubShapeTransform(shape.GetSubShape(i));
                result.emplace_back(shape.GetSubShape(i), shapeTransform, areaType);
            }
            return result;
        }

        std::vector<ChildRecastMeshObject> makeChildrenObjects(const JPH::Shape& shape, const AreaType areaType)
        {
            if (dynamic_cast<const JPH::CompoundShape*>(&shape))
                return makeChildrenObjects(static_cast<const JPH::CompoundShape&>(shape), areaType);
            return {};
        }
    }

    ChildRecastMeshObject::ChildRecastMeshObject(
        const JPH::CompoundShape::SubShape& shape, const osg::Matrixd& transform, const AreaType areaType)
        : mShape(shape)
        , mTransform(transform)
        , mAreaType(areaType)
        , mLocalScaling(osg::Vec3f(1.0f, 1.0f, 1.0f)) // jolt doesnt need to use localscaling
        , mChildren(makeChildrenObjects(*shape.mShape.GetPtr(), mAreaType))
    {
    }

    inline bool updateMeshObject(const JPH::RefConst<JPH::Shape>& actualShape, const osg::Matrixd& transform,
        const AreaType areaType, osg::Matrixd& mTransform, AreaType& mAreaType,
        std::vector<ChildRecastMeshObject>& mChildren)
    {
        bool result = false;
        if (!(mTransform == transform))
        {
            mTransform = transform;
            result = true;
        }
        if (mAreaType != areaType)
        {
            mAreaType = areaType;
            result = true;
        }

        // TODO: restore? jolt doesnt really support this tho
        // if (!(mLocalScaling == mShape.get().getLocalScaling()))
        // {
        //     mLocalScaling = mShape.get().getLocalScaling();
        //     result = true;
        // }

        if (dynamic_cast<const JPH::CompoundShape*>(actualShape.GetPtr()))
            result = updateCompoundObject(
                         *static_cast<const JPH::CompoundShape*>(actualShape.GetPtr()), mAreaType, mChildren)
                || result;

        return result;
    }

    bool ChildRecastMeshObject::update(const osg::Matrixd& transform, const AreaType areaType)
    {
        const JPH::RefConst<JPH::Shape> actualShape = mShape.get().mShape;
        return updateMeshObject(actualShape, transform, areaType, mTransform, mAreaType, mChildren);
    }

    bool RecastMeshObject::update(const osg::Matrixd& transform, const AreaType areaType)
    {
        return updateMeshObject(mShape, transform, areaType, mTransform, mAreaType, mChildren);
    }

    RecastMeshObject::RecastMeshObject(
        const CollisionShape& shape, const osg::Matrixd& transform, const AreaType areaType)
        : mInstance(shape.getInstance())
        , mObjectTransform(shape.getObjectTransform())
        , mShape(&shape.getShape())
        , mTransform(transform)
        , mAreaType(areaType)
        , mLocalScaling(osg::Vec3f(1.0f, 1.0f, 1.0f)) // jolt doesnt need to use localscaling
        , mChildren(makeChildrenObjects(*mShape.GetPtr(), mAreaType))
    {
    }
}
