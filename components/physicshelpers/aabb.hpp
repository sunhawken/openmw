#ifndef OPENMW_COMPONENTS_JOLTHELPERS_AABB_H
#define OPENMW_COMPONENTS_JOLTHELPERS_AABB_H

#include <osg/Matrixd>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <components/misc/convert.hpp>

namespace PhysicsSystemHelpers
{
    inline JPH::AABox getAabb(const JPH::Shape& shape, const osg::Matrixd& transform)
    {
        const JPH::Vec3 inScale = JPH::Vec3(1.0f, 1.0f, 1.0f);
        JPH::AABox bounds = shape.GetWorldSpaceBounds(Misc::Convert::toJoltNoScale(transform), inScale);
        return bounds;
    }
}

#endif
