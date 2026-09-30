#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <osg/FrameStamp>
#include <osg/Geode>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include "apps/openmw/mwrender/jiggleglute.hpp"

namespace MWRender
{
    namespace
    {
        TEST(JiggleBoneControllerTest, GluteNaturalisDefaultsKeepSharedFeel)
        {
            const JiggleGlute::Response r = JiggleGlute::computeResponse(0.f, 70.f, 0.f);
            EXPECT_NEAR(r.mTangentialStiffness, 1.f, 1e-5f);
            EXPECT_NEAR(r.mTangentialDamping, 1.f, 1e-5f);
            EXPECT_NEAR(r.mDepthStiffness, 1.f, 1e-5f);
            EXPECT_NEAR(r.mDepthDamping, 1.f, 1e-5f);
            EXPECT_NEAR(r.mMass, 1.f, 1e-5f);
            EXPECT_NEAR(r.mDepthIn, 1.f, 1e-5f);
            EXPECT_NEAR(r.mDepthOut, 1.f, 1e-5f);
            EXPECT_NEAR(r.mMaxDisplacement, 1.f, 1e-5f);
        }

        TEST(JiggleBoneControllerTest, GluteSofterGlutesLoosenSpringsAndDamping)
        {
            const JiggleGlute::Response firm = JiggleGlute::computeResponse(0.f, 20.f, 0.f);
            const JiggleGlute::Response soft = JiggleGlute::computeResponse(0.f, 100.f, 0.f);
            EXPECT_GT(firm.mTangentialStiffness, soft.mTangentialStiffness);
            EXPECT_GT(firm.mDepthStiffness, soft.mDepthStiffness);
            EXPECT_GT(firm.mTangentialDamping, soft.mTangentialDamping);
            EXPECT_LT(firm.mDepthOut, soft.mDepthOut);
        }

        TEST(JiggleBoneControllerTest, GluteQuicknessOffsetStiffensAndSlownessSoftens)
        {
            const float base = JiggleGlute::computeResponse(0.5f, 70.f, 0.f).mTangentialStiffness;
            EXPECT_GT(JiggleGlute::computeResponse(0.5f, 70.f, 1.f).mTangentialStiffness, base);
            EXPECT_LT(JiggleGlute::computeResponse(0.5f, 70.f, -1.f).mTangentialStiffness, base);
        }

        TEST(JiggleBoneControllerTest, GluteHeavierGlutesAreHeavierAndTravelFurther)
        {
            const JiggleGlute::Response light = JiggleGlute::computeResponse(0.f, 70.f, 0.f);
            const JiggleGlute::Response heavy = JiggleGlute::computeResponse(1.f, 70.f, 0.f);
            EXPECT_GT(heavy.mMass, light.mMass);
            EXPECT_GT(heavy.mMaxDisplacement, light.mMaxDisplacement);
            EXPECT_GT(heavy.mDepthOut, light.mDepthOut);
            EXPECT_LT(heavy.mDepthIn, light.mDepthIn);
            // Naturalis: 3 kg glute keeps 78% of its mass on the joint.
            EXPECT_NEAR(JiggleGlute::jointMass(1.f), 0.78f * 3.f, 1e-5f);
        }


    }
}
