#ifndef OPENMW_MWKURENAI_DRIVER_H
#define OPENMW_MWKURENAI_DRIVER_H

// KURENAI: Red Ninja's original player code, running in PCSX2, drives the player character.
//
// The driver owns the bridge region (OpenMW creates it, PCSX2 connects). Each frame it sends the player's input to
// PCSX2 as a PS2 pad state and places the player where the game put Kurenai. While PCSX2 is not connected,
// Morrowind's own controls work as usual. Settings: [Kurenai] in settings.cfg (see settings-default.cfg).

#include <kurenai/combat.hpp>

#include <osg/Vec3f>

#include "../mwworld/ptr.hpp"
#include "poseoverride.hpp"

#include <kurenai/link.hpp>
#include <kurenai/player.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace MyGUI
{
    class TextBox;
}

namespace MWKurenai
{
    class Driver
    {
    public:
        // Opens the bridge if [Kurenai] enabled is true; otherwise the driver does nothing.
        Driver();
        ~Driver();

        // After Lua has applied the frame's controls, before mechanics: forward input, take control of the player.
        void beforeMechanics(float dt);
        // After physics, before the world update: place the player from the latest PlayerState.
        void afterPhysics(float dt);

        bool connected() const { return mPeerAlive; }

        // Held by the engine while physics and the world update run; the bridge thread answers the game's collision
        // queries (Jolt ray casts) only outside those windows.
        std::unique_lock<std::mutex> lockWorld() { return std::unique_lock<std::mutex>(mWorldMutex); }

    private:
        void serviceThread();
        void answer(const kurenai::proto::CollisionQuery& q);
        bool send(const kurenai::proto::Payload& p);
        void onConnect();
        void onDisconnect();
        void sendInput();
        void applyAnimation(std::uint32_t motion);
        void handleCombat(const kurenai::proto::CombatEvent& e);
        void handlePose(const kurenai::proto::Pose& p);
        void updatePoseOverride();
        void applyPendingRagdolls();

        std::unique_ptr<kurenai::Link> mLink;
        kurenai::CoordTransform mTransform;
        bool mAnchored = false;
        bool mPeerAlive = false;
        bool mCollisionWasOn = false;
        bool mFollowGround = true; // [Kurenai] height mode = ground
        bool mInterpolate = true;
        std::uint64_t mPeerTimeoutNs = 2'000'000'000; // PCSX2 can stall while it loads a stage
        float mRnGroundY = 0; // Red Ninja height of Kurenai's last ground contact
        float mMwZ = 0; // last applied Morrowind height

        // The two latest states, for interpolation between the game's 30 Hz frames.
        std::optional<kurenai::proto::PlayerState> mPrev;
        std::optional<kurenai::proto::PlayerState> mLatest;
        float mSinceLatest = 0;
        std::uint64_t mStatesReceived = 0;
        std::uint64_t mFrames = 0;

        kurenai::proto::Input mLastInput;
        float mSinceInput = 0;
        std::uint32_t mLastMotion = 0xffffffffu;
        std::FILE* mLog = nullptr;

        // The bridge thread owns the incoming ring: it answers CollisionQuery at once and queues the rest for the
        // main thread. Both threads send, so sends are serialised.
        std::thread mThread;
        std::atomic<bool> mStop{ false };
        std::mutex mSendMutex;
        std::mutex mInboxMutex;
        std::deque<kurenai::proto::Message> mInbox;
        std::mutex mWorldMutex;
        std::mutex mTransformMutex; // guards mTransform and mAnchored for the bridge thread
        bool mAnswerCollision = false;

        // Combat ([Kurenai] combat): who each attack has hit already, and killing blows whose ragdoll should start
        // with the blade's impulse once Morrowind's combat code has applied the damage.
        bool mCombat = false;
        std::uint32_t mLastAttack = 0;
        std::uint32_t mLoggedAttack = 0;
        std::vector<const void*> mHitThisAttack;
        struct PendingRagdoll
        {
            MWWorld::Ptr actor;
            osg::Vec3f impulse;
            int framesLeft;
        };
        std::vector<PendingRagdoll> mPendingRagdolls;
        std::uint64_t mCombatEvents = 0, mCombatHits = 0;

        // PS2 animation ([Kurenai] ps2 animation): the game's skeleton, retargeted onto the player's.
        enum class Ps2Anim
        {
            Off,
            Locomotion,
            All
        } mPs2Anim = Ps2Anim::Off;
        std::optional<kurenai::SkeletonMap> mSkeletonMap;
        std::shared_ptr<PoseTarget> mPoseTarget = std::make_shared<PoseTarget>();
        osg::ref_ptr<PoseOverride> mPoseCallback;
        osg::Node* mPoseSkeleton = nullptr;
        std::uint64_t mPosesReceived = 0;

        // Game camera ([Kurenai] camera mode = game): Red Ninja's camera, converted to Morrowind's world.
        bool mGameCameraMode = false;
        bool mCameraTaken = false;
        int mPreviousCameraMode = 2;
        void applyGameCamera();
        void releaseCamera();

        // Latency of PlayerState from PCSX2's publish to arrival here (same monotonic clock on both sides).
    public:
        struct LatencyStats
        {
            std::uint64_t count = 0;
            double sumUs = 0, maxUs = 0;
            double lastUs = 0;
        };
        const LatencyStats& latency() const { return mLatency; }

    private:
        LatencyStats mLatency;
        std::uint64_t mLastPeerFrame = 0;

        // [Kurenai] show status: one text line in the corner.
        bool mShowStatus = false;
        MyGUI::TextBox* mStatus = nullptr;
        float mSinceStatus = 0;
        void updateStatus(float dt);
        std::atomic<std::uint64_t> mQueries{ 0 }, mQueryHits{ 0 }, mQueriesDeclined{ 0 }, mQueryNs{ 0 };
    };
}

#endif
