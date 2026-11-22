#ifndef OPENMW_MWLUA_LIMBCONTACT_H
#define OPENMW_MWLUA_LIMBCONTACT_H

#include <algorithm>
#include <cmath>
#include <optional>
#include <osg/Vec3f>
#include <components/esm3/loadarmo.hpp>

namespace MWLua::LimbContact
{
    inline bool finite(const osg::Vec3f& p)
    { return std::isfinite(p.x()) && std::isfinite(p.y()) && std::isfinite(p.z()); }

    struct Legs
    {
        osg::Vec3f pelvis, axis;
        float lower, upper;
        bool contains(const osg::Vec3f& point, int part) const
        {
            if (!finite(point) || part == ESM::PRT_Head || part == ESM::PRT_Hair || part == ESM::PRT_Neck)
                return false;
            // A posed knee can fold across the pelvis-to-neck plane. An actual
            // rendered leg surface remains a leg regardless of that projection.
            if (part >= ESM::PRT_RFoot && part <= ESM::PRT_LLeg)
                return true;
            const float height = (point - pelvis) * axis;
            // This is an accepted hit on this NPC already. The entire leg area
            // counts; mesh labels and a narrow distance to the knee do not veto it.
            return height >= lower && height <= upper;
        }
    };

    inline std::optional<Legs> legs(const osg::Vec3f& pelvis, const osg::Vec3f& neck,
        const osg::Vec3f& leftFoot, const osg::Vec3f& rightFoot)
    {
        if (!finite(pelvis) || !finite(neck) || !finite(leftFoot) || !finite(rightFoot)) return {};
        osg::Vec3f axis = neck - pelvis;
        const float torso = axis.normalize();
        if (!std::isfinite(torso) || torso < 1.f) return {};
        return Legs{pelvis, axis, std::min((leftFoot-pelvis)*axis, (rightFoot-pelvis)*axis) - 12.f,
            std::clamp(torso * .15f, 6.f, 10.f)};
    }

    inline float segmentDistance2(const osg::Vec3f& point, const osg::Vec3f& a, const osg::Vec3f& b)
    {
        const osg::Vec3f ab = b-a;
        const float length2 = ab.length2();
        const float t = length2 > .0001f ? std::clamp(((point-a)*ab)/length2, 0.f, 1.f) : 0.f;
        return (point-a-ab*t).length2();
    }

    inline unsigned closestArm(const osg::Vec3f& point,
        const osg::Vec3f& leftShoulder, const osg::Vec3f& leftElbow, const osg::Vec3f& leftHand,
        const osg::Vec3f& rightShoulder, const osg::Vec3f& rightElbow, const osg::Vec3f& rightHand)
    {
        if (!finite(point) || !finite(leftShoulder) || !finite(leftElbow) || !finite(leftHand)
            || !finite(rightShoulder) || !finite(rightElbow) || !finite(rightHand)) return 0;
        const float left = std::min(segmentDistance2(point,leftShoulder,leftElbow),
            segmentDistance2(point,leftElbow,leftHand));
        const float right = std::min(segmentDistance2(point,rightShoulder,rightElbow),
            segmentDistance2(point,rightElbow,rightHand));
        return left <= right ? 4u : 8u;
    }
}
#endif
