#include <gtest/gtest.h>

#include <osg/FrameStamp>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include "apps/openmw/mwrender/jigglebonecontroller.hpp"

namespace MWRender
{
    namespace
    {
        TEST(JiggleBoneControllerTest, CapeStyleVirtualPointRespondsToParentMotion)
        {
            osg::ref_ptr<osg::MatrixTransform> parent = new osg::MatrixTransform(osg::Matrix::identity());
            osg::ref_ptr<osg::MatrixTransform> bone = new osg::MatrixTransform(osg::Matrix::identity());
            bone->setName("Bip01 CapeTest");
            parent->addChild(bone);

            WiggleBoneSettings settings;
            settings.mDirect = true;
            settings.mActive = true;
            settings.mStiffness = 100.f;
            settings.mDamping = 10.f;
            settings.mAmplitude = 1.f;
            settings.mGravity = 0.f;
            settings.mMass = 1.f;
            settings.mStretch = 3.f;
            settings.mSelfCollision = false;
            settings.mSimulationOffset = osg::Vec3f(-4.f, 0.f, -18.f);

            osg::ref_ptr<JiggleBoneController> controller = new JiggleBoneController(false, false, settings);

            osg::ref_ptr<osg::FrameStamp> frame = new osg::FrameStamp;
            osg::NodeVisitor nv(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);
            nv.setFrameStamp(frame.get());

            // First update captures the authored rest transform.
            frame->setSimulationTime(0.0);
            (*controller)(bone.get(), &nv);
            EXPECT_TRUE(bone->getMatrix().getTrans().valid());
            EXPECT_NEAR(bone->getMatrix().getTrans().length(), 0.f, 1e-5f);

            // Move the parent as the actor root/torso would move in-game. The virtual point
            // should lag in world space, producing a non-zero local translation on the cape bone.
            parent->setMatrix(osg::Matrix::translate(10.f, 0.f, 0.f));
            frame->setSimulationTime(1.0 / 60.0);
            (*controller)(bone.get(), &nv);

            const osg::Vec3f moved = bone->getMatrix().getTrans();
            EXPECT_TRUE(moved.valid());
            EXPECT_GT(moved.length(), 0.01f);
        }

        TEST(JiggleBoneControllerTest, DisabledDirectBoneReturnsToAuthoredRest)
        {
            osg::ref_ptr<osg::MatrixTransform> parent = new osg::MatrixTransform(osg::Matrix::identity());
            osg::ref_ptr<osg::MatrixTransform> bone = new osg::MatrixTransform(osg::Matrix::identity());
            bone->setName("Bip01 CapeTest");
            parent->addChild(bone);

            WiggleBoneSettings settings;
            settings.mDirect = true;
            settings.mActive = false;
            settings.mGravity = 0.f;
            settings.mSimulationOffset = osg::Vec3f(-4.f, 0.f, -18.f);

            osg::ref_ptr<JiggleBoneController> controller = new JiggleBoneController(false, false, settings);
            osg::ref_ptr<osg::FrameStamp> frame = new osg::FrameStamp;
            osg::NodeVisitor nv(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);
            nv.setFrameStamp(frame.get());

            frame->setSimulationTime(0.0);
            (*controller)(bone.get(), &nv);
            parent->setMatrix(osg::Matrix::translate(10.f, 0.f, 0.f));
            frame->setSimulationTime(1.0 / 60.0);
            (*controller)(bone.get(), &nv);

            EXPECT_NEAR(bone->getMatrix().getTrans().length(), 0.f, 1e-5f);
        }
    }
}
