#include <apps/openmw/mwphysics/ragdollpose.hpp>
#include <gtest/gtest.h>
#include <limits>

using namespace MWPhysics;

TEST(RagdollPose, RoundTripsBothFormatsInRotatedActorFrame)
{
    const auto actor = osg::Matrixf::rotate(.7f, osg::Vec3f(0, 0, 1)) * osg::Matrixf::translate(200, 300, 400);
    for (unsigned mask : { 0u, 1u, 2u, 4u, 8u, 15u })
    {
        std::vector<osg::Matrixf> pose(mask ? 11 : 2);
        for (unsigned i = 0; i < pose.size(); ++i)
            pose[i] = osg::Matrixf::rotate(.1f * i, osg::Vec3f(1, 0, 0)) * osg::Matrixf::translate(i * 10.f, 20, 30)
                * actor;
        const auto bytes = RagdollPose::encode(pose, actor, mask);
        ASSERT_EQ(bytes.size(), mask ? 316u : 60u);
        EXPECT_EQ(bytes.substr(0, 4), mask ? "LSP1" : "BSP1");
        const auto restored = RagdollPose::decode(bytes, actor, mask);
        ASSERT_TRUE(restored);
        for (unsigned i = 0; i < pose.size(); ++i)
            for (unsigned element = 0; element < 16; ++element)
                EXPECT_NEAR(pose[i].ptr()[element], (*restored)[i].ptr()[element], .0002f);
    }
}

TEST(RagdollPose, RejectsMalformedPayloadBeforeRestoringAnyBody)
{
    const osg::Matrixf actor;
    std::vector<osg::Matrixf> pose(11);
    const auto good = RagdollPose::encode(pose, actor, 3);
    ASSERT_EQ(good.size(), 316u);
    for (unsigned n = 0; n < good.size(); ++n)
        EXPECT_FALSE(RagdollPose::decode(good.substr(0, n), actor, 3));
    EXPECT_FALSE(RagdollPose::decode(good + "x", actor, 3));
    EXPECT_FALSE(RagdollPose::decode(good, actor, 1));
    EXPECT_FALSE(RagdollPose::decode(good, actor, 16));
    auto corrupt = good;
    corrupt[0] = 'X';
    EXPECT_FALSE(RagdollPose::decode(corrupt, actor, 3));
    for (float bad : { std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 131073.f })
    {
        corrupt = good;
        std::string value;
        RagdollPose::append(value, std::bit_cast<std::uint32_t>(bad));
        corrupt.replace(8, 4, value);
        EXPECT_FALSE(RagdollPose::decode(corrupt, actor, 3));
    }
    corrupt = good;
    corrupt.replace(8 + 24, 4, std::string(4, '\0'));
    EXPECT_FALSE(RagdollPose::decode(corrupt, actor, 3));
}

TEST(RagdollPose, RejectsNonRigidFramesAndOutOfRangeOffsets)
{
    std::vector<osg::Matrixf> pose(2);
    EXPECT_TRUE(RagdollPose::encode(pose, osg::Matrixf::scale(-1, 1, 1), 0).empty());
    EXPECT_TRUE(RagdollPose::encode(pose, osg::Matrixf::scale(2, 1, 1), 0).empty());
    pose[1].setTrans(131073, 0, 0);
    EXPECT_TRUE(RagdollPose::encode(pose, osg::Matrixf(), 0).empty());
}
