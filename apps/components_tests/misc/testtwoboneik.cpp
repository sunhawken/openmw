#include <components/misc/twoboneik.hpp>

#include <osg/io_utils>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

MATCHER_P2(Vec3fEq, other, precision, "")
{
    return std::abs(arg.x() - other.x()) < precision && std::abs(arg.y() - other.y()) < precision
        && std::abs(arg.z() - other.z()) < precision;
}

namespace testing
{
    template <>
    inline testing::Message& Message::operator<<(const osg::Vec3f& value)
    {
        return (*this) << "osg::Vec3f(" << value.x() << ", " << value.y() << ", " << value.z() << ')';
    }
}

namespace Misc
{
    namespace
    {
        using namespace testing;

        constexpr float sPrecision = 1e-4f;

        // root=(0,0,0), rootLength=3, middleLength=4, target at distance 5 along +X, pole along +Y.
        // This is a 3-4-5 right triangle with the right angle at the middle joint, so the middle
        // joint position is exactly solvable by hand: (1.8, 2.4, 0).
        TEST(MiscSolveTwoBoneIKTest, reachableTargetMatchesHandSolvedTriangle)
        {
            const auto result = solveTwoBoneIK(osg::Vec3f(0, 0, 0), 3.f, 4.f, osg::Vec3f(5, 0, 0), osg::Vec3f(0, 1, 0));

            EXPECT_THAT(result.middle, Vec3fEq(osg::Vec3f(1.8f, 2.4f, 0.f), sPrecision));
            EXPECT_THAT(result.end, Vec3fEq(osg::Vec3f(5, 0, 0), sPrecision));
            EXPECT_NEAR(result.stretch, 1.f, sPrecision);

            // The solved pose must actually be consistent with the requested bone lengths.
            EXPECT_NEAR((result.middle - osg::Vec3f(0, 0, 0)).length(), 3.f, sPrecision);
            EXPECT_NEAR((result.end - result.middle).length(), 4.f, sPrecision);
        }

        // Bending towards the pole on the opposite side should mirror the middle joint's position.
        TEST(MiscSolveTwoBoneIKTest, poleOnOppositeSideMirrorsMiddleJoint)
        {
            const auto result
                = solveTwoBoneIK(osg::Vec3f(0, 0, 0), 3.f, 4.f, osg::Vec3f(5, 0, 0), osg::Vec3f(0, -1, 0));

            EXPECT_THAT(result.middle, Vec3fEq(osg::Vec3f(1.8f, -2.4f, 0.f), sPrecision));
            EXPECT_THAT(result.end, Vec3fEq(osg::Vec3f(5, 0, 0), sPrecision));
        }

        // A fully extended reach (target exactly at rootLength + middleLength) must leave the chain
        // perfectly straight: middle lies exactly on the root->target line.
        TEST(MiscSolveTwoBoneIKTest, fullyExtendedReachIsStraight)
        {
            const auto result = solveTwoBoneIK(osg::Vec3f(0, 0, 0), 3.f, 4.f, osg::Vec3f(7, 0, 0), osg::Vec3f(0, 1, 0));

            EXPECT_THAT(result.middle, Vec3fEq(osg::Vec3f(3, 0, 0), sPrecision));
            EXPECT_THAT(result.end, Vec3fEq(osg::Vec3f(7, 0, 0), sPrecision));
            EXPECT_NEAR(result.stretch, 1.f, sPrecision);
        }

        // A target beyond the chain's natural reach must stretch proportionally rather than fall
        // short: the end effector always reaches exactly the target.
        TEST(MiscSolveTwoBoneIKTest, outOfReachTargetStretchesToReachExactly)
        {
            const auto result
                = solveTwoBoneIK(osg::Vec3f(0, 0, 0), 3.f, 4.f, osg::Vec3f(10, 0, 0), osg::Vec3f(0, 1, 0));

            const float expectedStretch = 10.f / 7.f;
            EXPECT_NEAR(result.stretch, expectedStretch, sPrecision);
            EXPECT_THAT(result.end, Vec3fEq(osg::Vec3f(10, 0, 0), sPrecision));

            // Chain stays straight when fully stretched, and both segments scale by the same factor.
            EXPECT_THAT(result.middle, Vec3fEq(osg::Vec3f(3.f * expectedStretch, 0, 0), sPrecision));
            EXPECT_NEAR((result.middle - osg::Vec3f(0, 0, 0)).length(), 3.f * expectedStretch, sPrecision);
            EXPECT_NEAR((result.end - result.middle).length(), 4.f * expectedStretch, sPrecision);
        }

        // A target much closer than the chain can fold (inside |rootLength - middleLength|) must
        // not produce NaNs or infinities; exact bend geometry is not asserted since this is a
        // degenerate configuration that real skeletons should rarely if ever hit.
        TEST(MiscSolveTwoBoneIKTest, tooCloseTargetStaysFinite)
        {
            const auto result
                = solveTwoBoneIK(osg::Vec3f(0, 0, 0), 3.f, 4.f, osg::Vec3f(0.5f, 0, 0), osg::Vec3f(0, 1, 0));

            EXPECT_TRUE(std::isfinite(result.middle.x()) && std::isfinite(result.middle.y())
                && std::isfinite(result.middle.z()));
            EXPECT_THAT(result.end, Vec3fEq(osg::Vec3f(0.5f, 0, 0), sPrecision));
            EXPECT_NEAR(result.stretch, 1.f, sPrecision);
        }

        // A pole that is exactly colinear with the root->target axis gives no bend-plane
        // information; the solver must fall back to an arbitrary perpendicular direction instead
        // of producing NaNs.
        TEST(MiscSolveTwoBoneIKTest, degeneratePoleStaysFinite)
        {
            const auto result
                = solveTwoBoneIK(osg::Vec3f(0, 0, 0), 3.f, 4.f, osg::Vec3f(5, 0, 0), osg::Vec3f(10, 0, 0));

            EXPECT_TRUE(std::isfinite(result.middle.x()) && std::isfinite(result.middle.y())
                && std::isfinite(result.middle.z()));
            EXPECT_THAT(result.end, Vec3fEq(osg::Vec3f(5, 0, 0), sPrecision));
            EXPECT_NEAR((result.middle - osg::Vec3f(0, 0, 0)).length(), 3.f, sPrecision);
            EXPECT_NEAR((result.end - result.middle).length(), 4.f, sPrecision);
        }

        // Root position should act as a pure origin offset: translating everything (root, target,
        // pole) by the same vector should translate the result by that same vector.
        TEST(MiscSolveTwoBoneIKTest, translationInvariant)
        {
            const osg::Vec3f offset(12.f, -7.f, 3.5f);
            const auto base = solveTwoBoneIK(osg::Vec3f(0, 0, 0), 3.f, 4.f, osg::Vec3f(5, 0, 0), osg::Vec3f(0, 1, 0));
            const auto shifted
                = solveTwoBoneIK(offset, 3.f, 4.f, osg::Vec3f(5, 0, 0) + offset, osg::Vec3f(0, 1, 0) + offset);

            EXPECT_THAT(shifted.middle, Vec3fEq(base.middle + offset, sPrecision));
            EXPECT_THAT(shifted.end, Vec3fEq(base.end + offset, sPrecision));
            EXPECT_NEAR(shifted.stretch, base.stretch, sPrecision);
        }
    }
}
