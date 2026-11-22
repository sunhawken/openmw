// Derived from sunspotdark/morrowind-modern-physics
// (https://github.com/sunspotdark/morrowind-modern-physics, commit eb50a40,
// apps/openmw/mwphysics/ragdoll.hpp), GPLv3; see LICENSE.
// Native Jolt backend for Bru NPC/creature ragdolls, knockdowns and persistent dismemberment.
#ifndef OPENMW_MWPHYSICS_RAGDOLL_H
#define OPENMW_MWPHYSICS_RAGDOLL_H
#include "ptrholder.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <osg/Matrixf>
#include <osg/Vec3f>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MWPhysics
{
    class PhysicsTaskScheduler;
    struct CreatureRig;
    class Ragdoll
    {
    public:
        using BoneMatrices = std::map<std::string, osg::Matrixf, std::less<>>;
        using BoneMotion = std::map<std::string, std::pair<osg::Vec3f, osg::Vec3f>, std::less<>>;
        using ExtremityVertices = std::map<std::string, std::vector<osg::Vec3f>, std::less<>>;
        using UnsupportedExtremityBones = std::map<std::string, unsigned, std::less<>>;
        enum class KickProfile
        {
            Chest,
            LowTrip
        };
        static const std::vector<std::string>& getRequiredBones();
        static BoneMotion sampleAnimationMotion(const BoneMatrices& previous, const BoneMatrices& current,
            const osg::Vec3f& actorTranslation, float duration);
        Ragdoll(const MWWorld::Ptr& actor, const BoneMatrices& bones, const osg::Vec3f& velocity,
            const osg::Vec3f& kick, PhysicsTaskScheduler* scheduler,
            const std::optional<osg::Vec3f>& contact = std::nullopt, const BoneMotion& motion = {},
            const ExtremityVertices& surfaceVertices = {}, const UnsupportedExtremityBones& unsupportedBones = {},
            KickProfile kickProfile = KickProfile::Chest);
        // A creature's body from its planned rig: no anatomical drives, cuts or equipment release.
        Ragdoll(const MWWorld::Ptr& actor, const CreatureRig& rig, const BoneMatrices& bones,
            const osg::Vec3f& velocity, const osg::Vec3f& kick, PhysicsTaskScheduler* scheduler,
            const std::optional<osg::Vec3f>& contact = std::nullopt, KickProfile kickProfile = KickProfile::Chest);
        ~Ragdoll();
        // Render-only interpolation; omitted alpha uses the latest completed pose.
        // Physical transforms and velocities remain the exact latest completed Jolt step.
        std::vector<std::pair<std::string, osg::Matrixf>> getBonePoses(
            std::optional<float> interpolation = std::nullopt) const;
        std::optional<osg::Vec3f> getActorPosition(const osg::Vec3f& actorPosition);
        const std::vector<std::pair<std::string, osg::Matrixf>>& getLastBonePoses() const { return mLastPoses; }
        void updatePtr(const MWWorld::Ptr& updated);
        MWWorld::Ptr getPtr() const;
        const std::vector<JPH::BodyID>& getCollisionBodies() const { return mBodies; }
        std::size_t partCount() const { return mParts.size(); }
        float maxJointError() const;
        std::vector<osg::Vec3f> getDynamics() const;
        float bottom() const;
        float retainedBottom() const;
        void recordLanding(float height, float damage)
        {
            ++mLandingCount;
            mLastFallHeight = height;
            mLastFallDamage = damage;
        }
        osg::Vec3f getLandingState() const { return osg::Vec3f(mLandingCount, mLastFallHeight, mLastFallDamage); }
        float initialRotationError() const { return mInitialRotationError; }
        float initialMatrixError() const { return mInitialMatrixError; }
        float speed() const { return mMaxSpeed; }
        float angularSpeed() const { return mMaxAngularSpeed; }
        // Zero cut velocity restores a saved severed state without relaunching.
        bool detachHead(const osg::Vec3f& cutVelocity, const osg::Vec3f& headAngularImpulse);
        bool detachTorso(
            const osg::Vec3f& cutVelocity, const osg::Vec3f& upperSeparation, const std::string& savedPose = {});
        std::string getBisectionPoseData() const;
        bool detachLimbs(unsigned mask, const osg::Vec3f& cutVelocity, const std::string& savedPose = {});
        std::string getLimbSeveringPoseData() const;
        unsigned getSeveredLimbsMask() const { return mSeveredLimbsMask; }
        bool isTorsoDetached() const { return mTorsoDetached; }
        std::vector<osg::Vec3f> getHeadState() const;
        // Nearest unobstructed collision surface in the kick cone, using the
        // current physical pose. Returns its distance, or a negative value.
        float findKickPart(const osg::Vec3f& origin, const osg::Vec3f& forward, float reach, float minimumDot,
            std::size_t& part, osg::Vec3f& point) const;
        bool isDetachedHeadPart(std::size_t part) const;
        // One bounded momentum change for exactly the struck connected fragment.
        bool applyKick(std::size_t part, const osg::Vec3f& velocity, const osg::Vec3f& point);
        int kickCount() const { return mKickCount; }
        std::map<std::string, double> getEnhancementState() const;
        bool applyImpact(std::size_t part, const osg::Vec3f& point, const osg::Vec3f& impulse, std::uint64_t id,
            bool alreadyApplied = false);
        void configureEnhancement();
        void beforeStep(float duration);
        void setWater(bool enabled, float height)
        {
            mWaterEnabled = enabled;
            mWaterHeight = height;
        }
        bool getEquipmentFrame(bool left, osg::Matrixf& frame, osg::Vec3f& linear, osg::Vec3f& angular) const;
        std::vector<JPH::BodyID> getEquipmentGripBodies() const;
        bool startSuperKick(
            std::uint64_t token, std::uint64_t kickId, const osg::Vec3f& velocity, JPH::BodyID attacker);
        std::map<std::string, double> getSuperKickState() const;
        void guardSuperKickSpawn();
        struct SuperKickContact
        {
            std::uint64_t token;
            JPH::BodyID actor;
            osg::Vec3f transfer;
        };
        std::vector<SuperKickContact> takeSuperKickContacts();

    private:
        struct Part;
        void build(const MWWorld::Ptr&, const CreatureRig&, const BoneMatrices&, const osg::Vec3f&, const BoneMotion&,
            const ExtremityVertices& = {});
        void refresh() const;
        void updateFragmentCollisions();
        bool detach(unsigned child);
        unsigned fragment(unsigned part) const;
        std::string savePose(bool torso) const;
        bool restorePose(std::string_view bytes, bool torso, unsigned mask);
        PhysicsTaskScheduler* mTaskScheduler;
        JPH::PhysicsSystem* mSystem;
        JPH::Ref<JPH::RagdollSettings> mSettings;
        JPH::Ref<JPH::GroupFilterTable> mGroupFilter;
        std::vector<std::pair<unsigned, unsigned>> mSeparatedPairs;
        struct RagdollDeleter
        {
            void operator()(JPH::Ragdoll*) const;
        };
        std::unique_ptr<JPH::Ragdoll, RagdollDeleter> mRagdoll;
        std::vector<std::unique_ptr<Part>> mParts;
        std::vector<JPH::BodyID> mBodies;
        mutable std::vector<std::pair<std::string, osg::Matrixf>> mLastPoses;
        mutable std::vector<osg::Vec3f> mLinear, mAngular;
        mutable std::vector<osg::Matrixf> mTransforms;
        std::vector<osg::Matrixf> mPreviousTransforms;
        osg::Vec3f mActorOffset, mInitialRootVelocity, mInheritedVelocity, mImpulse, mContact, mInitialCOMVelocity;
        mutable float mMaxSpeed = 0, mMaxAngularSpeed = 0;
        float mInitialPoseError = 0, mInitialRotationError = 0, mInitialMatrixError = 0;
        float mLandingCount = 0, mLastFallHeight = 0, mLastFallDamage = 0;
        bool mHeadDetached = false, mTorsoDetached = false, mCreature = false;
        unsigned mSeveredLimbsMask = 0;
        float mKickScale = 1, mWaterHeight = 0, mAge = 0;
        bool mWaterEnabled = false, mPoseRestored = false, mPoseRejected = false;
        std::uint64_t mSuperKickToken = 0;
        float mSuperKickAge = 0;
        JPH::BodyID mSuperKickAttacker;
        std::mutex mContactMutex;
        std::vector<SuperKickContact> mSuperKickContacts;
        std::unordered_set<unsigned> mSuperKickHitBodies;
        osg::Vec3f mHeadDetachErrors, mHeadDetachVelocity, mHeadDetachAngularVelocity;
        int mKickCount = 0;
        std::unordered_set<std::uint64_t> mImpacts;
        UnsupportedExtremityBones mUnsupportedExtremityBones;
    };
}
#endif
