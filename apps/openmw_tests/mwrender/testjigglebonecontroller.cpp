#include <gtest/gtest.h>

#include <algorithm>

#include <osg/FrameStamp>
#include <osg/Geode>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include "apps/openmw/mwrender/jigglebonecontroller.hpp"
#include "apps/openmw/mwrender/jiggleautorig.hpp"
#include "components/sceneutil/riggeometry.hpp"
#include "components/sceneutil/skeleton.hpp"

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
            settings.mSimulationOffset = osg::Vec3f(0.f, -8.f, -18.f);

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

        TEST(JiggleBoneControllerTest, CapeStyleVirtualPointRespondsToParentRotation)
        {
            osg::ref_ptr<osg::MatrixTransform> parent = new osg::MatrixTransform(osg::Matrix::identity());
            osg::ref_ptr<osg::MatrixTransform> bone = new osg::MatrixTransform(osg::Matrix::identity());
            bone->setName("Bip01 CapeTurnTest");
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
            settings.mUseBodyResponse = false;
            settings.mSimulationOffset = osg::Vec3f(0.f, -8.f, -18.f);

            osg::ref_ptr<JiggleBoneController> controller = new JiggleBoneController(false, false, settings);
            osg::ref_ptr<osg::FrameStamp> frame = new osg::FrameStamp;
            osg::NodeVisitor nv(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);
            nv.setFrameStamp(frame.get());

            frame->setSimulationTime(0.0);
            (*controller)(bone.get(), &nv);

            // A torso turn rotates the virtual cloth point around the parent. Its simulated
            // world position should lag, producing a local cape displacement even without
            // translating the actor.
            parent->setMatrix(osg::Matrix::rotate(0.5f, osg::Vec3f(0.f, 0.f, 1.f)));
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
            settings.mSimulationOffset = osg::Vec3f(0.f, -8.f, -18.f);

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

        TEST(JiggleBoneControllerTest, RoseSorceressAutoRigInjectsCapeBonesAndPreservesArmCloth)
        {
            osg::ref_ptr<SceneUtil::Skeleton> skeleton = new SceneUtil::Skeleton;

            osg::ref_ptr<osg::MatrixTransform> spine = new osg::MatrixTransform(osg::Matrix::identity());
            spine->setName("Bip01 Spine2");
            skeleton->addChild(spine);

            osg::ref_ptr<osg::MatrixTransform> upperArm = new osg::MatrixTransform(osg::Matrix::identity());
            upperArm->setName("Bip01 L UpperArm");
            skeleton->addChild(upperArm);

            osg::ref_ptr<osg::Geometry> source = new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            // Three hanging-cloth samples from shoulder to hem plus one sleeve sample.
            vertices->push_back(osg::Vec3f(0.f, -8.f, 110.f));
            vertices->push_back(osg::Vec3f(0.f, -14.f, 80.f));
            vertices->push_back(osg::Vec3f(0.f, -24.f, 30.f));
            vertices->push_back(osg::Vec3f(-30.f, -2.f, 105.f));
            source->setVertexArray(vertices);

            osg::ref_ptr<SceneUtil::RigGeometry> rig = new SceneUtil::RigGeometry;
            rig->setName("Tri Groin Cape 0");
            rig->setSourceGeometry(source);

            std::vector<SceneUtil::RigGeometry::BoneInfo> boneInfo(2);
            boneInfo[0].mName = "bip01 spine2";
            boneInfo[0].mInvBindMatrix = osg::Matrixf::identity();
            boneInfo[1].mName = "bip01 l upperarm";
            boneInfo[1].mInvBindMatrix = osg::Matrixf::identity();
            rig->setBoneInfo(std::move(boneInfo));

            std::vector<SceneUtil::RigGeometry::BoneWeights> influences(vertices->size());
            influences[0].emplace_back(0, 1.f);
            influences[1].emplace_back(0, 1.f);
            influences[2].emplace_back(0, 1.f);
            influences[3].emplace_back(1, 1.f);
            rig->setInfluences(influences);

            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            geode->setName("Tri Groin Cape 0");
            geode->setUserValue("meshFileName", std::string("meshes/rosesorceress/rose_sorceress_cape.nif"));
            geode->addDrawable(rig);
            skeleton->addChild(geode);

            // Cape compatibility must work even with female-body auto-rigging disabled.
            JiggleAutoRig::run(skeleton.get(), false, false);

            SceneUtil::Bone* capeBone01 = skeleton->getBone("bip01 cape01");
            SceneUtil::Bone* capeBone02 = skeleton->getBone("bip01 cape02");
            SceneUtil::Bone* capeBone03 = skeleton->getBone("bip01 cape03");
            ASSERT_NE(capeBone01, nullptr);
            ASSERT_NE(capeBone02, nullptr);
            ASSERT_NE(capeBone03, nullptr);

            // All generated segments must share the authored torso pivot. Nesting the
            // identity bones would stack translations and exaggerate lower-cape motion.
            ASSERT_NE(capeBone01->mNode.get(), nullptr);
            ASSERT_NE(capeBone02->mNode.get(), nullptr);
            ASSERT_NE(capeBone03->mNode.get(), nullptr);
            ASSERT_EQ(capeBone01->mNode->getNumParents(), 1u);
            ASSERT_EQ(capeBone02->mNode->getNumParents(), 1u);
            ASSERT_EQ(capeBone03->mNode->getNumParents(), 1u);
            EXPECT_EQ(capeBone01->mNode->getParent(0), spine.get());
            EXPECT_EQ(capeBone02->mNode->getParent(0), spine.get());
            EXPECT_EQ(capeBone03->mNode->getParent(0), spine.get());

            // Existence alone is not enough: every generated cape segment must be driven
            // by a runtime update callback so the skinned vertices can actually lag/move.
            EXPECT_NE(capeBone01->mNode->getUpdateCallback(), nullptr);
            EXPECT_NE(capeBone02->mNode->getUpdateCallback(), nullptr);
            EXPECT_NE(capeBone03->mNode->getUpdateCallback(), nullptr);

            const std::vector<std::string> names = rig->getInfluenceBoneNames();
            auto boneIndexByName = [&](std::string_view name) {
                const auto it = std::find(names.begin(), names.end(), name);
                return it == names.end() ? names.size() : static_cast<std::size_t>(std::distance(names.begin(), it));
            };
            const std::size_t cape01 = boneIndexByName("bip01 cape01");
            const std::size_t cape02 = boneIndexByName("bip01 cape02");
            const std::size_t cape03 = boneIndexByName("bip01 cape03");
            ASSERT_LT(cape01, names.size());
            ASSERT_LT(cape02, names.size());
            ASSERT_LT(cape03, names.size());

            const auto painted = rig->getPerVertexInfluences(vertices->size());
            auto capeWeight = [&](std::size_t vertex) {
                float total = 0.f;
                for (const auto& [bone, weight] : painted[vertex])
                    if (bone == cape01 || bone == cape02 || bone == cape03)
                        total += weight;
                return total;
            };

            EXPECT_NEAR(capeWeight(0), 0.f, 1e-5f); // shoulder attachment remains pinned
            EXPECT_GT(capeWeight(1), 0.f);          // hanging mid-cape gets procedural motion
            EXPECT_GT(capeWeight(2), 0.8f);        // hem gets the strongest response
            EXPECT_NEAR(capeWeight(3), 0.f, 1e-5f); // sleeve remains authored to UpperArm

            const std::size_t boneCountAfterFirstPass = rig->getBoneInfoList().size();
            JiggleAutoRig::run(skeleton.get(), false, false);
            EXPECT_EQ(rig->getBoneInfoList().size(), boneCountAfterFirstPass);
        }

    }
}
