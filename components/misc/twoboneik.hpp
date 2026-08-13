#ifndef MISC_TWOBONEIK_H
#define MISC_TWOBONEIK_H

#include <osg/Quat>
#include <osg/Vec3f>

#include <algorithm>
#include <cmath>

namespace Misc
{
    struct TwoBoneIKResult
    {
        //! New world-space position of the middle joint (elbow/knee).
        osg::Vec3f middle;

        //! New world-space position of the end effector (hand/foot). Always exactly \a target:
        //! when the target is out of reach the chain is stretched rather than falling short.
        osg::Vec3f end;

        //! Ratio applied to both bone lengths to reach an out-of-reach target. 1 when the target
        //! is within the chain's natural reach.
        float stretch = 1.f;
    };

    /// Solves a two-bone (e.g. shoulder-elbow-hand, or hip-knee-foot) IK chain so the end effector
    /// reaches \a target as closely as possible.
    ///
    /// Unlike a typical clamped IK solve, when \a target is farther than \a rootLength +
    /// \a middleLength allow, the chain is stretched proportionally so the end effector still
    /// reaches \a target exactly. This models the "stretchy limb" look common in VR avatars that
    /// lack real elbow/knee tracking.
    ///
    /// \param root root of the chain (shoulder/hip), in world space. Does not move.
    /// \param rootLength length of the root-to-middle segment (upper arm/thigh) in the bind pose.
    /// \param middleLength length of the middle-to-end segment (forearm/shin) in the bind pose.
    /// \param target desired world-space position of the end effector.
    /// \param pole a world-space point the middle joint should bend towards. Only its direction
    ///        from \a root, projected perpendicular to the root-target axis, is used, so it does
    ///        not need to be an exact or reachable position.
    inline TwoBoneIKResult solveTwoBoneIK(const osg::Vec3f& root, float rootLength, float middleLength,
        const osg::Vec3f& target, const osg::Vec3f& pole)
    {
        constexpr float epsilon = 1e-4f;

        TwoBoneIKResult result;

        const osg::Vec3f toTarget = target - root;
        const float targetLen = toTarget.length();

        const float maxLen = rootLength + middleLength;
        const float minLen = std::abs(rootLength - middleLength);

        if (targetLen > maxLen)
            result.stretch = targetLen / std::max(maxLen, epsilon);

        // Distance to use for the interior-angle computation below: the real target distance when
        // reachable, or the chain's full extension when it must stretch (which yields a shoulder
        // angle of exactly zero, i.e. a straight arm).
        const float angleLen = std::clamp(targetLen, minLen + epsilon, maxLen);

        osg::Vec3f targetDir = targetLen > epsilon ? toTarget / targetLen : osg::Vec3f(0, 1, 0);

        // Interior angle at the root between the root->middle segment and the root->target axis,
        // via the law of cosines.
        float cosRootAngle
            = (rootLength * rootLength + angleLen * angleLen - middleLength * middleLength) / (2.f * rootLength * angleLen);
        cosRootAngle = std::clamp(cosRootAngle, -1.f, 1.f);
        const float rootAngle = std::acos(cosRootAngle);

        // Direction from root to the pole, made perpendicular to targetDir: this defines the plane
        // the whole chain bends in.
        const osg::Vec3f poleDir = pole - root;
        osg::Vec3f bendDir = poleDir - targetDir * (poleDir * targetDir);
        float bendDirLen = bendDir.length();
        if (bendDirLen < epsilon)
        {
            // Pole is degenerate (colinear with the root-target axis); fall back to an arbitrary
            // perpendicular direction so the solve stays well-defined.
            const osg::Vec3f fallback = std::abs(targetDir.z()) < 0.99f ? osg::Vec3f(0, 0, 1) : osg::Vec3f(1, 0, 0);
            bendDir = fallback - targetDir * (fallback * targetDir);
            bendDirLen = bendDir.length();
        }
        bendDir /= bendDirLen;

        // Axis to rotate targetDir around so it sweeps towards bendDir (i.e. towards the pole).
        osg::Vec3f hingeAxis = targetDir ^ bendDir;
        const float hingeAxisLen = hingeAxis.length();
        hingeAxis = hingeAxisLen > epsilon ? hingeAxis / hingeAxisLen : osg::Vec3f(1, 0, 0);

        const osg::Quat rootRotation(rootAngle, hingeAxis);
        const osg::Vec3f middleDir = rootRotation * targetDir;

        result.middle = root + middleDir * (rootLength * result.stretch);
        result.end = target;

        return result;
    }
}

#endif
