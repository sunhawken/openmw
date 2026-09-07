#ifndef OPENMW_COMPONENTS_JOLTHELPERS_HEIGHTFIELD_H
#define OPENMW_COMPONENTS_JOLTHELPERS_HEIGHTFIELD_H

#include <osg/Vec3>

namespace PhysicsSystemHelpers
{
    inline osg::Vec3f getHeightfieldShift(int x, int y, int size, float minHeight, float maxHeight)
    {
        return osg::Vec3f((x + 0.5f) * size, (y + 0.5f) * size, (maxHeight + minHeight) * 0.5f);
    }
}

#endif
