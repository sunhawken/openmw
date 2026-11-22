// Jolt requires its platform definitions before every other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/RegisterTypes.h>
#include <apps/openmw/mwclass/npc.hpp>
#include <apps/openmw/mwphysics/joltlayers.hpp>
#include <apps/openmw/mwphysics/joltlisteners.hpp>
#include <apps/openmw/mwphysics/mtphysics.hpp>
#include <apps/openmw/mwphysics/ragdoll.hpp>
#include <apps/openmw/mwphysics/ragdollpose.hpp>
#include <apps/openmw/mwworld/livecellref.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <gtest/gtest.h>

namespace
{
    class JoltBruRagdoll : public testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            JPH::RegisterDefaultAllocator();
            JPH::Factory::sInstance = new JPH::Factory;
            JPH::RegisterTypes();
            MWClass::Npc::registerSelf();
        }
        static void TearDownTestSuite()
        {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
        MWPhysics::JoltBPLayerInterface broad;
        MWPhysics::JoltObjectVsBroadPhaseLayerFilter broadFilter;
        MWPhysics::JoltObjectLayerPairFilter pairs;
        MWPhysics::JoltContactListener listener;
        JPH::PhysicsSystem system;
        JPH::JobSystemSingleThreaded jobs{ JPH::cMaxPhysicsJobs };
        JPH::TempAllocatorImpl memory{ 16 * 1024 * 1024 };
        std::unique_ptr<MWPhysics::PhysicsTaskScheduler> scheduler;
        ESM::NPC npc;
        std::unique_ptr<MWWorld::LiveCellRef<ESM::NPC>> live;
        MWWorld::Ptr ptr;
        MWPhysics::Ragdoll::BoneMatrices bones;
        void SetUp() override
        {
            system.Init(1024, 0, 2048, 2048, broad, broadFilter, pairs);
            system.SetContactListener(&listener);
            system.SetGravity(JPH::Vec3(0, 0, -627.2f));
            scheduler = std::make_unique<MWPhysics::PhysicsTaskScheduler>(1.f / 60, &system, nullptr, &jobs);
            npc.blank();
            npc.mId = ESM::RefId::stringRefId("ragdoll_test");
            ESM::CellRef ref;
            ref.blank();
            ref.mRefID = npc.mId;
            live = std::make_unique<MWWorld::LiveCellRef<ESM::NPC>>(ref, &npc);
            ptr = MWWorld::Ptr(live.get());
            ptr.getRefData().setBaseNode(new SceneUtil::PositionAttitudeTransform);
            const auto bone = [&](const char* name, float x, float y, float z) {
                bones.emplace(name, osg::Matrixf::translate(x, y, z));
            };
            bone("Bip01 Pelvis", 0, 0, 90);
            bone("Bip01 Spine1", 0, 0, 112);
            bone("Bip01 Neck", 0, 0, 140);
            bone("Bip01 Head", 0, 0, 148);
            for (const auto& side : { std::pair{ "L", -1.f }, std::pair{ "R", 1.f } })
            {
                const auto add = [&](const char* name, float x, float z) {
                    const auto full = std::string("Bip01 ") + side.first + " " + name;
                    bone(full.c_str(), x * side.second, 0, z);
                };
                add("UpperArm", 18, 133);
                add("Forearm", 42, 124);
                add("Hand", 63, 115);
                add("Thigh", 9, 88);
                add("Calf", 9, 47);
                add("Foot", 9, 10);
            }
        }
        std::unique_ptr<MWPhysics::Ragdoll> make()
        {
            return std::make_unique<MWPhysics::Ragdoll>(ptr, bones, osg::Vec3f(), osg::Vec3f(), scheduler.get());
        }
        void step(MWPhysics::Ragdoll& ragdoll, unsigned count)
        {
            for (unsigned i = 0; i < count; ++i)
            {
                ragdoll.beforeStep(1.f / 60);
                EXPECT_EQ(system.Update(1.f / 60, 1, &memory, &jobs), JPH::EPhysicsUpdateError::None);
            }
        }
    };

    TEST_F(JoltBruRagdoll, SimulatesElevenBodiesAndRemovesThemOnDestruction)
    {
        auto body = make();
        ASSERT_EQ(body->partCount(), 11u);
        ASSERT_EQ(system.GetNumBodies(), 11u);
        step(*body, 30);
        EXPECT_LT(body->maxJointError(), 3.f);
        for (const auto& [name, matrix] : body->getBonePoses())
            for (unsigned i = 0; i < 16; ++i)
                EXPECT_TRUE(std::isfinite(matrix.ptr()[i]));
        body.reset();
        EXPECT_EQ(system.GetNumBodies(), 0u);
    }

    TEST_F(JoltBruRagdoll, DetachesHeadWithoutDeletingRetainedBodies)
    {
        auto body = make();
        ASSERT_TRUE(body->detachHead({ 0, 100, 100 }, { 0, 1, 0 }));
        EXPECT_EQ(body->getHeadState()[0].x(), 1.f);
        EXPECT_EQ(body->getHeadState()[0].y(), 9.f);
        EXPECT_EQ(system.GetNumBodies(), 11u);
        step(*body, 20);
        EXPECT_FALSE(body->detachTorso({}, {}, {}));
        EXPECT_FALSE(body->detachLimbs(1, {}, {}));
        EXPECT_LT(body->maxJointError(), 3.f);
    }

    TEST_F(JoltBruRagdoll, RestoresEverySeveredLimbAndRejectsBadPayload)
    {
        auto body = make();
        ASSERT_TRUE(body->detachLimbs(15, { 0, 100, 0 }));
        step(*body, 8);
        const auto saved = body->getLimbSeveringPoseData();
        ASSERT_EQ(saved.size(), 316u);
        body.reset();
        body = make();
        ASSERT_TRUE(body->detachLimbs(15, {}, saved));
        EXPECT_EQ(body->getSeveredLimbsMask(), 15u);
        EXPECT_EQ(body->getEnhancementState().at("limbPoseRestored"), 1);
        const auto expected = MWPhysics::RagdollPose::decode(saved, osg::Matrixf(), 15);
        const auto restored = MWPhysics::RagdollPose::decode(body->getLimbSeveringPoseData(), osg::Matrixf(), 15);
        ASSERT_TRUE(expected);
        ASSERT_TRUE(restored);
        for (unsigned i = 0; i < 11; ++i)
            for (unsigned j = 0; j < 16; ++j)
                EXPECT_NEAR((*expected)[i].ptr()[j], (*restored)[i].ptr()[j], .001f);
        body.reset();
        body = make();
        ASSERT_TRUE(body->detachLimbs(15, {}, "invalid"));
        EXPECT_EQ(body->getEnhancementState().at("limbPosePayloadRejected"), 1);
    }

    TEST_F(JoltBruRagdoll, RestoresTorsoFragments)
    {
        auto body = make();
        ASSERT_TRUE(body->detachTorso({ 0, 100, 0 }, { 0, 0, 40 }));
        step(*body, 8);
        const auto saved = body->getBisectionPoseData();
        ASSERT_EQ(saved.size(), 60u);
        body.reset();
        body = make();
        ASSERT_TRUE(body->detachTorso({}, {}, saved));
        EXPECT_EQ(body->getEnhancementState().at("torsoPoseRestored"), 1);
        EXPECT_LT(body->maxJointError(), 3.f);
    }
    TEST_F(JoltBruRagdoll, LandsOnFloorWithoutTunnelling)
    {
        auto& interface = system.GetBodyInterface();
        const auto floor = interface.CreateAndAddBody(
            JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(1000, 1000, 5)), JPH::RVec3(0, 0, -5),
                JPH::Quat::sIdentity(), JPH::EMotionType::Static, MWPhysics::Layers::WORLD),
            JPH::EActivation::DontActivate);
        auto body = make();
        step(*body, 180);
        EXPECT_GT(body->bottom(), -2.f);
        EXPECT_LT(body->bottom(), 10.f);
        EXPECT_LT(body->maxJointError(), 3.f);
        body.reset();
        interface.RemoveBody(floor);
        interface.DestroyBody(floor);
    }

    TEST_F(JoltBruRagdoll, SuperKickReportsOneContactPerActor)
    {
        auto& interface = system.GetBodyInterface();
        const auto target = interface.CreateAndAddBody(
            JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(40, 10, 80)), JPH::RVec3(0, 70, 90),
                JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, MWPhysics::Layers::ACTOR),
            JPH::EActivation::Activate);
        auto body = make();
        ASSERT_TRUE(body->startSuperKick(42, 42, { 0, 900, 0 }, {}));
        step(*body, 12);
        const auto contacts = body->takeSuperKickContacts();
        ASSERT_EQ(contacts.size(), 1u);
        EXPECT_EQ(contacts[0].token, 42u);
        EXPECT_EQ(contacts[0].actor, target);
        EXPECT_GT(contacts[0].transfer.y(), 0.f);
        EXPECT_TRUE(body->takeSuperKickContacts().empty());
        body.reset();
        interface.RemoveBody(target);
        interface.DestroyBody(target);
    }

    TEST_F(JoltBruRagdoll, OverlappingSecondarySpawnDoesNotExplode)
    {
        auto first=make();
        auto second=make();
        second->guardSuperKickSpawn();
        for(unsigned i=0;i<30;++i)
        {
            first->beforeStep(1.f/60);second->beforeStep(1.f/60);
            EXPECT_EQ(system.Update(1.f/60,1,&memory,&jobs),JPH::EPhysicsUpdateError::None);
        }
        EXPECT_LT(first->speed(),400.f);EXPECT_LT(second->speed(),400.f);
        EXPECT_LT(first->maxJointError(),3.f);EXPECT_LT(second->maxJointError(),3.f);
    }

}
