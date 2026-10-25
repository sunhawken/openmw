#ifndef OPENMW_MWKURENAI_DRIVER_H
#define OPENMW_MWKURENAI_DRIVER_H

// KURENAI: Red Ninja's original player code, running in PCSX2, drives the player character.
//
// The driver owns the bridge region (OpenMW creates it, PCSX2 connects). Each frame it sends the player's input to
// PCSX2 as a PS2 pad state and places the player where the game put Kurenai. While PCSX2 is not connected,
// Morrowind's own controls work as usual. Settings: [Kurenai] in settings.cfg (see settings-default.cfg).

#include <kurenai/combat.hpp>

#include <osg/Vec3f>
#include <osg/Array>
#include <osg/observer_ptr>

#include "../mwworld/ptr.hpp"
#include "poseoverride.hpp"

#include <kurenai/hud.hpp>
#include <kurenai/link.hpp>
#include <kurenai/player.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace osg
{
    class Geometry;
}

namespace MyGUI
{
    class ImageBox;
    class ITexture;
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
        // Combat mode: PCSX2 drives her. Morrowind mode: Morrowind does, while PCSX2 stays connected and idle.
        bool driving() const { return mControlling; }

        // Held by the engine while physics and the world update run; the bridge thread answers the game's collision
        // queries (Jolt ray casts) only outside those windows.
        std::unique_lock<std::mutex> lockWorld() { return std::unique_lock<std::mutex>(mWorldMutex); }

    private:
        void serviceThread();
        void answer(const kurenai::proto::CollisionQuery& q);
        bool send(const kurenai::proto::Payload& p);
        void takeControl();
        void releaseControl(const char* why);
        void pollModeKey();
        void sendInput();
        void applyAnimation(std::uint32_t motion);
        void handleCombat(const kurenai::proto::CombatEvent& e);
        void sendProxyTargets(const kurenai::CoordTransform& t);
        void handleProxyStatus(const kurenai::proto::ProxyStatus& s);
        void handleProxyPose(const kurenai::proto::ProxyPose& p);
        void updateVictims();
        void handlePose(const kurenai::proto::Pose& p);
        void updatePoseOverride();
        void applyPendingRagdolls();

        std::unique_ptr<kurenai::Link> mLink;
        kurenai::CoordTransform mTransform;
        bool mAnchored = false;
        bool mPeerAlive = false;
        bool mMorrowindMode = false; // [Kurenai] start mode, switched with [Kurenai] mode toggle key
        bool mControlling = false; // PCSX2 drives the player (connected and in combat mode)
        int mToggleScancode = 0; // SDL scancode, 0 = no key
        bool mToggleHeld = false;
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
        bool mForcingSneak = false; // we set Flag_ForceSneak (her crouch / wall hug, the anim map's sneak line)
        void updateSneak(std::uint32_t motion);
        std::FILE* mLog = nullptr;

        // The bridge thread owns the incoming ring: it answers CollisionQuery at once and queues the rest for the
        // main thread. Both threads send, so sends are serialised.
        std::thread mThread;
        std::atomic<bool> mStop{ false };
        std::mutex mSendMutex;
        std::mutex mInboxMutex;
        struct Arrived
        {
            kurenai::proto::Message message;
            std::uint64_t arrivalNs; // when the bridge thread took it off the ring
        };
        std::deque<Arrived> mInbox;
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

        // Proxies ([Kurenai] proxies): the game's enemies standing in for the actors around her. Ids are ours; the
        // game reports per id what it did to the proxy.
        bool mProxyEnabled = true;
        float mProxyRadius = 3000;
        struct Proxy
        {
            MWWorld::Ptr actor;
            std::int32_t lastHealth = 0; // the game's health of its proxy when we last applied damage
            bool assigned = false; // the game has an enemy on it
            bool killed = false;
        };
        std::map<std::uint32_t, Proxy> mProxies;
        std::map<const void*, std::uint32_t> mProxyIds; // actor (LiveCellRefBase) -> id
        std::uint32_t mNextProxyId = 1;
        std::uint32_t mLockedProxy = 0; // id Kurenai is locked onto, 0 = none
        std::uint64_t mProxyKills = 0, mProxyDamage = 0;
        // Kill sync: the victim of her stealth kill moves as the game's enemy does (ProxyPose): its skeleton follows
        // the enemy's through the enemy's skeleton map, Morrowind's own animation (and the death ragdoll) held off
        // until the body lies still, then the last pose is kept.
        struct Victim
        {
            MWWorld::Ptr actor;
            std::shared_ptr<PoseTarget> target = std::make_shared<PoseTarget>();
            osg::ref_ptr<PoseOverride> callback;
            osg::observer_ptr<osg::Node> rootBone;
            bool final = false;
            std::uint64_t poses = 0;
        };
        std::map<std::uint32_t, Victim> mVictims; // by proxy id

        // PS2 animation ([Kurenai] ps2 animation): the game's skeleton, retargeted onto the player's.
        enum class Ps2Anim
        {
            Off,
            Locomotion,
            All
        } mPs2Anim = Ps2Anim::Off;
        std::map<std::uint32_t, kurenai::SkeletonMap> mSkeletonMaps; // by model CRC (0: maps without a model line)
        std::uint32_t mPoseModel = 0xffffffffu; // the model of the last pose (logged when it changes)
        std::vector<kurenai::proto::Vec3> mPoseBones; // the last pose, Red Ninja world

        // Her wire, drawn while the game has it out: from her hand (the game's) to where it is fixed.
        osg::ref_ptr<osg::Geometry> mWireGeometry;
        osg::ref_ptr<osg::Vec3Array> mWireVertices;
        void updateWire(const kurenai::CoordTransform& t);
        std::shared_ptr<PoseTarget> mPoseTarget = std::make_shared<PoseTarget>();
        osg::ref_ptr<PoseOverride> mPoseCallback;
        osg::observer_ptr<osg::Node> mPoseSkeleton; // the animation object can be rebuilt; never keep it alive
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
            double transportSumUs = 0, transportMaxUs = 0; // PCSX2 publish -> bridge thread
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

        // [Kurenai] game hud: Red Ninja's own HUD, cut out of each PCSX2 frame (HudSurface), drawn at the same
        // corners of the screen, scaled with the window's height.
        std::unique_ptr<kurenai::HudSurface> mHud;
        std::uint64_t mHudSeq = 0;
        kurenai::HudFrame mHudFrame;
        struct HudTexture
        {
            MyGUI::ITexture* texture = nullptr;
            std::string name;
            int size[2] = { 0, 0 };
        };
        std::vector<HudTexture> mHudTextures; // one per region
        std::vector<MyGUI::ImageBox*> mHudBoxes;
        float mHudScale = 1, mSinceHudFrame = 1e9f;
        bool mHideMorrowindHud = true, mMorrowindHudHidden = false;
        std::uint64_t mHudFrames = 0;
        void updateHud(float dt);
        void hideHud();
        std::atomic<std::uint64_t> mQueries{ 0 }, mQueryHits{ 0 }, mQueriesDeclined{ 0 }, mQueryNs{ 0 };
        std::atomic<std::uint64_t> mQueryLockNs{ 0 }, mQueryLockMaxNs{ 0 }; // time waiting for the world lock
    };
}

#endif
