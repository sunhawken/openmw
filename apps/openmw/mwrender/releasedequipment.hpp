#ifndef OPENMW_MWRENDER_RELEASEDEQUIPMENT_H
#define OPENMW_MWRENDER_RELEASEDEQUIPMENT_H

#include <cstdint>
#include <osg/Geometry>

namespace MWRender
{
    // Same identity check at capture and restore. Positions are deliberately
    // excluded: the saved arrays are the evaluated attachment pose.
    inline uint64_t releasedEquipmentTopologyHash(const osg::Geometry& geometry)
    {
        uint64_t result = 14695981039346656037ull;
        const auto add = [&](uint32_t value) {
            for (unsigned shift = 0; shift != 32; shift += 8)
            {
                result ^= (value >> shift) & 255u;
                result *= 1099511628211ull;
            }
        };
        add(geometry.getVertexArray() ? geometry.getVertexArray()->getNumElements() : 0);
        add(geometry.getNumPrimitiveSets());
        for (unsigned i = 0; i < geometry.getNumPrimitiveSets(); ++i)
        {
            const auto* primitive = geometry.getPrimitiveSet(i);
            add(primitive->getMode());
            add(primitive->getNumIndices());
            for (unsigned j = 0; j < primitive->getNumIndices(); ++j)
                add(primitive->index(j));
        }
        return result;
    }
}

#endif
