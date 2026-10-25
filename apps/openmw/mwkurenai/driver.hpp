#ifndef OPENMW_MWKURENAI_DRIVER_H
#define OPENMW_MWKURENAI_DRIVER_H

// KURENAI: Red Ninja's original player code, running in PCSX2, drives the player character.
//
// The driver owns the bridge region (OpenMW creates it, PCSX2 connects). Each frame it sends the player's input to
// PCSX2 as a PS2 pad state and places the player where the game put Kurenai. While PCSX2 is not connected,
// Morrowind's own controls work as usual. Settings: [Kurenai] in settings.cfg (see settings-default.cfg).

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
        std::atomic<std::uint64_t> mQueries{ 0 }, mQueryHits{ 0 }, mQueriesDeclined{ 0 }, mQueryNs{ 0 };
    };
}

#endif
