#ifndef OPENMW_MWRENDER_RELEASEDEQUIPMENTCOMPAT_H
#define OPENMW_MWRENDER_RELEASEDEQUIPMENTCOMPAT_H

#include <components/esm3/releasedequipmentstate.hpp>
#include <osg/Geometry>
#include <algorithm>
#include <array>
#include <cmath>

namespace MWRender
{
    // Compatibility with old pointer-ordered optimizer output. Do not remap
    // duplicate vertices: an unchanged static source can use its own arrays,
    // topology and UVs together, with the saved attachment residual transform.
    inline bool releasedEquipmentStaticVerticesMatch(const osg::Geometry& source,
        const ESM::ReleasedEquipmentState::Drawable& saved)
    {
        const auto* positions = dynamic_cast<const osg::Vec3Array*>(source.getVertexArray());
        const auto* normals = dynamic_cast<const osg::Vec3Array*>(source.getNormalArray());
        const auto* tangents = dynamic_cast<const osg::Vec4Array*>(source.getTexCoordArray(7));
        if (!positions || positions->empty() || positions->size() != saved.mVertices.size()
            || positions->size() != saved.mSourceVertexCount
            || positions->size() > ESM::ReleasedEquipmentState::MaxVertices) return false;
        const std::size_t normalCount = normals ? normals->size() : 0;
        const std::size_t tangentCount = tangents ? tangents->size() : 0;
        if (normalCount != saved.mNormals.size() || tangentCount != saved.mTangents.size()
            || (normalCount && normalCount != 1 && normalCount != positions->size())
            || (tangentCount && tangentCount != positions->size())
            || (source.getNormalArray() && !normals)
            || (source.getTexCoordArray(7) && !tangents)) return false;
        if (normalCount && int(source.getNormalBinding()) != int(normalCount == 1
                ? osg::Geometry::BIND_OVERALL : osg::Geometry::BIND_PER_VERTEX)) return false;
        if (tangentCount && int(tangents->getBinding()) != int(osg::Array::BIND_PER_VERTEX)) return false;
        using Key = std::array<float, 10>;
        std::vector<Key> current, recorded;
        current.reserve(positions->size()); recorded.reserve(positions->size());
        for (std::size_t i = 0; i < positions->size(); ++i)
        {
            Key a{}, b{};
            for (int j = 0; j < 3; ++j)
            { a[j] = (*positions)[i][j]; b[j] = saved.mVertices[i][j]; }
            if (normalCount)
                for (int j = 0; j < 3; ++j)
                { a[3+j] = (*normals)[normalCount == 1 ? 0 : i][j]; b[3+j] = saved.mNormals[normalCount == 1 ? 0 : i][j]; }
            if (tangentCount)
                for (int j = 0; j < 4; ++j)
                { a[6+j] = (*tangents)[i][j]; b[6+j] = saved.mTangents[i][j]; }
            for (unsigned j = 0; j < a.size(); ++j)
                if (!std::isfinite(a[j]) || !std::isfinite(b[j])) return false;
            current.push_back(a); recorded.push_back(b);
        }
        std::sort(current.begin(), current.end());
        std::sort(recorded.begin(), recorded.end());
        return current == recorded;
    }
}
#endif
