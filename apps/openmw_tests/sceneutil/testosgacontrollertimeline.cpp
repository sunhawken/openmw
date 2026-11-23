#include <components/sceneutil/osgacontroller.hpp>

#include <gtest/gtest.h>

#include <osgAnimation/Channel>

namespace
{
    using namespace SceneUtil;

    osg::Matrixf at(float x)
    {
        osg::Matrixf m = osg::Matrixf::identity();
        m.setTrans(x, 0.f, 0.f);
        return m;
    }

    // A channel moving `bone` along X from `fromX` at `t0` to `toX` at `t1`.
    osg::ref_ptr<osgAnimation::MatrixLinearChannel> channel(
        const std::string& bone, float t0, float fromX, float t1, float toX)
    {
        auto* keys = new osgAnimation::MatrixKeyframeContainer;
        keys->push_back(osgAnimation::MatrixKeyframe(t0, at(fromX)));
        keys->push_back(osgAnimation::MatrixKeyframe(t1, at(toX)));
        osg::ref_ptr<osgAnimation::MatrixLinearChannel> c = new osgAnimation::MatrixLinearChannel;
        c->setName("transform");
        c->setTargetName(bone);
        c->getOrCreateSampler()->setKeyframeContainer(keys);
        return c;
    }

    osg::ref_ptr<Resource::Animation> track(const std::string& name,
        std::initializer_list<osg::ref_ptr<osgAnimation::MatrixLinearChannel>> channels)
    {
        osg::ref_ptr<Resource::Animation> t = new Resource::Animation;
        t->setName(name);
        for (const auto& c : channels)
            t->addChannel(c.get());
        return t;
    }

    float x(const OsgAnimationController& c, float time, const std::string& bone)
    {
        return c.getTransformForNode(time, bone).getTrans().x();
    }

    TEST(OsgAnimationControllerTimelineTest, absoluteClipWithNonzeroStartSamplesItsOwnKeys)
    {
        OsgAnimationController c;
        c.addMergedAnimationTrack(track("first", { channel("a", 0.f, 0.f, 1.f, 10.f) }));
        c.addMergedAnimationTrack(track("second", { channel("a", 2.f, 100.f, 3.f, 200.f) }));
        c.setEmulatedAnimations({ { 0.f, 1.f, "first", true }, { 2.f, 3.f, "second", true } });

        EXPECT_FLOAT_EQ(x(c, 0.5f, "a"), 5.f);
        EXPECT_FLOAT_EQ(x(c, 2.f, "a"), 100.f);
        EXPECT_FLOAT_EQ(x(c, 2.5f, "a"), 150.f); // was frozen at the first key before
        EXPECT_FLOAT_EQ(x(c, 3.f, "a"), 200.f);
    }

    TEST(OsgAnimationControllerTimelineTest, lateChannelKeepsItsDelayInsideTheClip)
    {
        // Clip 2..4; bone "b" only starts moving at 3.
        OsgAnimationController c;
        c.addMergedAnimationTrack(
            track("clip", { channel("a", 2.f, 0.f, 4.f, 20.f), channel("b", 3.f, 0.f, 4.f, 10.f) }));
        c.setEmulatedAnimations({ { 2.f, 4.f, "clip", true } });

        EXPECT_FLOAT_EQ(x(c, 2.5f, "a"), 5.f);
        EXPECT_FLOAT_EQ(x(c, 2.5f, "b"), 0.f); // not yet started
        EXPECT_FLOAT_EQ(x(c, 3.5f, "b"), 5.f); // halfway through its own keys
        EXPECT_FLOAT_EQ(x(c, 4.f, "b"), 10.f);
    }

    TEST(OsgAnimationControllerTimelineTest, distinctClipsSharingOneSampler)
    {
        // Both clips point at the same keys (one sampler); each samples it at
        // the timeline time, inside its own window.
        osg::ref_ptr<osgAnimation::MatrixLinearChannel> shared = channel("a", 0.f, 0.f, 4.f, 40.f);
        OsgAnimationController c;
        c.addMergedAnimationTrack(track("early", { shared }));
        c.addMergedAnimationTrack(track("late", { shared }));
        c.setEmulatedAnimations({ { 0.f, 2.f, "early", true }, { 2.5f, 4.f, "late", true } });

        EXPECT_FLOAT_EQ(x(c, 1.f, "a"), 10.f);
        EXPECT_FLOAT_EQ(x(c, 3.f, "a"), 30.f);
    }

    TEST(OsgAnimationControllerTimelineTest, zeroAndNegativeStarts)
    {
        OsgAnimationController c;
        c.addMergedAnimationTrack(track("neg", { channel("a", -2.f, 0.f, -1.f, 10.f) }));
        c.addMergedAnimationTrack(track("zero", { channel("a", 0.f, 100.f, 1.f, 110.f) }));
        c.setEmulatedAnimations({ { -2.f, -1.f, "neg", true }, { 0.f, 1.f, "zero", true } });

        EXPECT_FLOAT_EQ(x(c, -1.5f, "a"), 5.f);
        EXPECT_FLOAT_EQ(x(c, 0.5f, "a"), 105.f);
    }

    TEST(OsgAnimationControllerTimelineTest, boundariesAndRestBetweenClips)
    {
        OsgAnimationController c;
        c.addMergedAnimationTrack(track("one", { channel("a", 0.f, 0.f, 1.f, 10.f) }));
        c.addMergedAnimationTrack(track("two", { channel("a", 2.f, 50.f, 3.f, 60.f) }));
        c.setEmulatedAnimations({ { 0.f, 1.f, "one", true }, { 2.f, 3.f, "two", true } });

        EXPECT_FLOAT_EQ(x(c, 0.f, "a"), 0.f); // first key, inclusive start
        EXPECT_FLOAT_EQ(x(c, 1.f, "a"), 10.f); // last key, inclusive stop
        EXPECT_EQ(c.getTransformForNode(1.5f, "a"), osg::Matrixf::identity()); // between clips: rest
        EXPECT_FLOAT_EQ(x(c, 2.f, "a"), 50.f); // switched to the other clip
        EXPECT_EQ(c.getTransformForNode(5.f, "a"), osg::Matrixf::identity()); // after all clips: rest
        EXPECT_FLOAT_EQ(x(c, 0.5f, "a"), 5.f); // and back again
    }

    TEST(OsgAnimationControllerTimelineTest, relativeKeyClipsUnchanged)
    {
        // Producers that store keys relative to the clip keep the old behaviour.
        OsgAnimationController c;
        c.addMergedAnimationTrack(track("rel", { channel("a", 0.1f, 0.f, 0.9f, 8.f) }));
        c.setEmulatedAnimations({ { 1.1f, 2.f, "rel" } });

        // 1.2 - 1.1 is not exactly 0.1 in floats, hence the tolerance.
        EXPECT_NEAR(x(c, 1.2f, "a"), 0.f, 1e-4f); // relative 0.1: first key
        EXPECT_NEAR(x(c, 1.6f, "a"), 4.f, 1e-4f); // relative 0.5: halfway
        EXPECT_NEAR(x(c, 2.f, "a"), 8.f, 1e-4f);
    }

    TEST(OsgAnimationControllerTimelineTest, unknownBoneIsIdentity)
    {
        OsgAnimationController c;
        c.addMergedAnimationTrack(track("one", { channel("a", 0.f, 0.f, 1.f, 10.f) }));
        c.setEmulatedAnimations({ { 0.f, 1.f, "one", true } });
        EXPECT_EQ(c.getTransformForNode(0.5f, "nobody"), osg::Matrixf::identity());
    }
}
