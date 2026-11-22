#ifndef OPENMW_MWPHYSICS_RAGDOLLPOSE_H
#define OPENMW_MWPHYSICS_RAGDOLLPOSE_H

#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <osg/Matrixf>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MWPhysics::RagdollPose
{
    inline bool valid(const osg::Matrixf& matrix)
    {
        for (unsigned i = 0; i < 16; ++i)
            if (!std::isfinite(matrix.ptr()[i]))
                return false;
        for (unsigned i = 0; i < 3; ++i)
        {
            osg::Vec3f row(matrix(i, 0), matrix(i, 1), matrix(i, 2));
            if (std::abs(row.length2() - 1.f) > .001f)
                return false;
            for (unsigned j = 0; j < i; ++j)
                if (std::abs(row * osg::Vec3f(matrix(j, 0), matrix(j, 1), matrix(j, 2))) > .001f)
                    return false;
        }
        const osg::Vec3f x(matrix(0, 0), matrix(0, 1), matrix(0, 2));
        const osg::Vec3f y(matrix(1, 0), matrix(1, 1), matrix(1, 2));
        const osg::Vec3f z(matrix(2, 0), matrix(2, 1), matrix(2, 2));
        if (std::abs((x ^ y) * z - 1.f) > .001f)
            return false;
        return std::abs(matrix(0, 3)) < .001f && std::abs(matrix(1, 3)) < .001f && std::abs(matrix(2, 3)) < .001f
            && std::abs(matrix(3, 3) - 1.f) < .001f;
    }

    inline void append(std::string& out, std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
            out.push_back(char((value >> shift) & 255));
    }

    // The BSP1/LSP1 wire format stays compatible with Bru saves across physics backends.
    inline std::string encode(std::span<const osg::Matrixf> world, const osg::Matrixf& actor, unsigned mask)
    {
        if (!valid(actor) || (mask ? world.size() != 11 || (mask & ~15u) : world.size() != 2))
            return {};
        std::string result(mask ? "LSP1" : "BSP1");
        if (mask)
            append(result, mask);
        const auto inverse = osg::Matrixf::inverse(actor);
        for (const auto& matrix : world)
        {
            if (!valid(matrix))
                return {};
            const auto relative = matrix * inverse;
            const auto p = relative.getTrans();
            auto q = relative.getRotate();
            q /= q.length();
            if (std::abs(p.x()) > 131072 || std::abs(p.y()) > 131072 || std::abs(p.z()) > 131072)
                return {};
            for (float value :
                { float(p.x()), float(p.y()), float(p.z()), float(q.x()), float(q.y()), float(q.z()), float(q.w()) })
                append(result, std::bit_cast<std::uint32_t>(value));
        }
        return result;
    }

    inline std::optional<std::vector<osg::Matrixf>> decode(
        std::string_view bytes, const osg::Matrixf& actor, unsigned mask)
    {
        const unsigned count = mask ? 11 : 2, header = mask ? 8 : 4;
        if ((mask & ~15u) || !valid(actor) || bytes.size() != header + count * 28
            || bytes.substr(0, 4) != (mask ? "LSP1" : "BSP1"))
            return {};
        unsigned cursor = 4;
        const auto read = [&]() {
            std::uint32_t value = 0;
            for (unsigned shift = 0; shift < 32; shift += 8)
                value |= std::uint32_t(static_cast<unsigned char>(bytes[cursor++])) << shift;
            return value;
        };
        if (mask && read() != mask)
            return {};
        std::vector<osg::Matrixf> result;
        for (unsigned i = 0; i < count; ++i)
        {
            float a[7];
            for (auto& value : a)
            {
                value = std::bit_cast<float>(read());
                if (!std::isfinite(value))
                    return {};
            }
            osg::Quat q(a[3], a[4], a[5], a[6]);
            if (std::abs(a[0]) > 131072 || std::abs(a[1]) > 131072 || std::abs(a[2]) > 131072
                || std::abs(q.length2() - 1) > .001)
                return {};
            q /= q.length();
            auto matrix = osg::Matrixf::rotate(q) * osg::Matrixf::translate(a[0], a[1], a[2]) * actor;
            if (!valid(matrix))
                return {};
            result.push_back(matrix);
        }
        return result;
    }
}
#endif
