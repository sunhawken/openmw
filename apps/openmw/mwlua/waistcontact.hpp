#ifndef OPENMW_MWLUA_WAISTCONTACT_H
#define OPENMW_MWLUA_WAISTCONTACT_H

#include <algorithm>
#include <cmath>
#include <optional>

#include <osg/Vec3f>
#include <components/esm3/loadarmo.hpp>

namespace MWLua::WaistContact
{
    inline bool finite(const osg::Vec3f& point)
    {
        return std::isfinite(point.x()) && std::isfinite(point.y()) && std::isfinite(point.z());
    }

    struct Region
    {
        osg::Vec3f center, axis;
        float halfWidth, lower, upper, radius;

        bool contains(const osg::Vec3f& point, int part) const
        {
            // A real head/neck mesh wins even in a bent pose. Other modular
            // labels must not veto a wrist, sleeve or garment over the waist.
            if (part == ESM::PRT_Head || part == ESM::PRT_Hair || part == ESM::PRT_Neck || !finite(point))
                return false;
            const auto delta = point - center;
            const float along = delta * axis;
            return along >= lower && along <= upper
                && (delta - axis * along).length2() <= radius * radius;
        }

        bool containsAimedContact(const osg::Vec3f& point, int part, const osg::Vec3f& origin,
            osg::Vec3f direction) const
        {
            if (contains(point, part)) return true;
            if (part == ESM::PRT_Head || part == ESM::PRT_Hair || part == ESM::PRT_Neck
                || !finite(point) || !finite(origin) || !finite(direction) || direction.normalize() < .001f)
                return false;
            // Perspective and the swing fan can touch the front of a garment
            // a little above/below the aimed internal waist. Rescue only a
            // nearby accepted contact; the view ray cannot create a new hit.
            const auto delta = point - center;
            const float along = delta * axis;
            if (along < lower - 6.f || along > upper + 6.f
                || (delta - axis * along).length2() > radius * radius) return false;
            // Match aim height at the torso axis. The 3D point nearest center
            // would slide a close, downward neck aim into the waist band.
            const auto radialDirection = direction - axis * (direction * axis);
            const float radialSpeed2 = radialDirection.length2();
            if (radialSpeed2 < .000001f) return false;
            const float distance = ((center - origin) * radialDirection) / radialSpeed2;
            return distance >= 0.f && distance <= (point - origin).length() + radius
                && contains(origin + direction * distance, part);
        }
    };

    inline std::optional<Region> make(const osg::Vec3f& pelvis, const osg::Vec3f& waist,
        const osg::Vec3f& neck)
    {
        if (!finite(pelvis) || !finite(waist) || !finite(neck)) return {};
        osg::Vec3f axis = neck - pelvis;
        const float span = axis.normalize();
        if (!std::isfinite(span) || span < 1.f) return {};
        // A broad lower-torso band, rather than a precise cut-plane hit. The
        // rendered cut still uses its independently validated Spine1 plane.
        const float halfWidth = std::clamp(span * .55f, 18.f, 28.f);
        const float lower = std::max(-halfWidth, (pelvis - waist) * axis - span * .2f);
        const float upper = std::min(halfWidth, (neck - waist) * axis - 3.f);
        if (lower >= upper) return {};
        return Region{ waist, axis, halfWidth, lower, upper, std::clamp(span * 1.1f, 32.f, 50.f) };
    }
}

#endif
