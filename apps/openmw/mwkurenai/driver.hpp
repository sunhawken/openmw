#ifndef OPENMW_MWKURENAI_DRIVER_H
#define OPENMW_MWKURENAI_DRIVER_H

// KURENAI: Red Ninja's original player code, running in PCSX2, drives the player character.
//
// The driver owns the bridge region (OpenMW creates it, PCSX2 connects). Each frame it sends the player's input to
// PCSX2 as a PS2 pad state and places the player where the game put Kurenai. While PCSX2 is not connected,
// Morrowind's own controls work as usual. Settings: [Kurenai] in settings.cfg (see settings-default.cfg).

#include <kurenai/combat.hpp>

#include <osg/Array>
#include <osg/MatrixTransform>
#include <osg/Vec3f>
#include <osg/observer_ptr>

#include <components/esm/refid.hpp>
#include <components/esm3/cellref.hpp>

#include "../mwworld/ptr.hpp"
#include "embeddedps2.hpp"
#include "hdui.hpp"
#include "nativebody.hpp"
#include "poseoverride.hpp"

#include <kurenai/hud.hpp>
#include <kurenai/link.hpp>
#include <kurenai/player.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct _SDL_Joystick;

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
        // The player's Morrowind death: while Red Ninja's code drives her, her own death motion plays first, and Morrowind's
        // death screen (load a save) waits for the game's death sequence to end (or 6 s). True = the caller must not open it.
        static bool deathDeferred();

        // The options menu (Lua: scripts/kurenai/menu.lua through openmw.kurenai). Every option is a list of choices; the
        // menu steps through them and the driver applies the pick at once and keeps it in the user's settings.
        struct Option
        {
            std::string id, label, value, kind = "choice"; // kind "key": value is an SDL key name, no choices
            std::vector<std::string> choices, names; // names: what each choice is called on screen
        };
        static Driver* instance();
        std::vector<Option> options();
        bool setOption(const std::string& id, const std::string& value);
        std::vector<std::string> padInfo() const;
        // While the menu is up the game gets neutral input; its pad events (D-pad, Cross, Circle, the open chord) queue here.
        void setMenuOpen(bool open) { mMenuOpen = open; mMenuPrevMask = 0xffff; }
        bool menuOpen() const { return mMenuOpen; }
        int menuKey() const { return mMenuScancode & 0xffff; }
        int menuKeyMods() const { return mMenuScancode >> 16; } // 1 Ctrl, 2 Shift, 4 Alt
        std::vector<std::string> takeNav();

        // Held by the engine while physics and the world update run; the bridge thread answers the game's collision
        // queries (Jolt ray casts) only outside those windows.
        std::unique_lock<std::timed_mutex> lockWorld() { return std::unique_lock<std::timed_mutex>(mWorldMutex); }

    private:
        void serviceThread();
        void answer(const kurenai::proto::CollisionQuery& q);
        void answerWater(const kurenai::proto::CollisionQuery& q, const kurenai::CoordTransform& t,
            const kurenai::proto::Vec3& a, const kurenai::proto::Vec3& b);
        void useSkill(ESM::RefId skill, int usage);
        bool mJumpHeld = false;
        void updateSwim(float dt, kurenai::proto::Vec3& mw);
        bool send(const kurenai::proto::Payload& p);
        void takeControl();
        void releaseControl(const char* why);
        void pollModeKey();
        void setMode(bool morrowind, const char* how);
        void pollRecoverKey();
        void pollControlsKey();
        void showControls();
        void sendInput();
        void applyAnimation(std::uint32_t motion);
        void handleCombat(const kurenai::proto::CombatEvent& e);
        bool proxied(const MWWorld::Ptr& actor) const;
        void handleDart(const kurenai::proto::CombatEvent& e, const osg::Vec3f& from, const osg::Vec3f& to);
        void sendProxyTargets(const kurenai::CoordTransform& t);
        void handleProxyStatus(const kurenai::proto::ProxyStatus& s);
        void handleProxyPose(const kurenai::proto::ProxyPose& p);
        void updateVictims();
        void handlePose(const kurenai::proto::Pose& p);
        void updatePoseOverride();
        void applyPendingRagdolls();

        std::unique_ptr<kurenai::Link> mLink;
        std::unique_ptr<EmbeddedPs2> mPs2; ///< The game inside OpenMW ([Kurenai] ps2 folder), else null
        kurenai::CoordTransform mTransform;
        bool mAnchored = false;
        bool mPeerAlive = false;
        bool mMorrowindMode = false; // [Kurenai] start mode, switched with [Kurenai] mode toggle key
        bool mControlling = false; // PCSX2 drives the player (connected and in combat mode)
        int parseKey(const std::string& text) const;
        std::string keyName(int code) const;
        bool keyIsDown(int code) const;
        int mToggleScancode = 0; // SDL scancode (+ modifiers in the high bits), 0 = no key
        int mRecoverScancode = 0; // [Kurenai] recover key, 0 = none
        bool mRecoverHeld = false;
        bool mRecoverNow = false; // recover on the next update (key, or the void watch)
        bool mHaveSafe = false; // mSafe: the last solid ground she stood on (Morrowind coordinates)
        osg::Vec3f mSafe;
        float mSafeTimer = 0.f, mVoidTime = 0.f;
        bool mReanchorAtSafe = false;
        bool mToggleHeld = false;
        bool mTogglePadHeld = false;
        float mSinceTake = 0.f;
        int mResyncs = 0;
        int mControlsScancode = 0; // [Kurenai] controls key, 0 = none
        int mSelectScancode = 0; // [Kurenai] select key, 0 = none
        int mWeaponScancode = 0; // [Kurenai] weapon key (the weapon screen), 0 = none
        bool mWeaponOpen = false;
        int mMenuScancode = 0; // [Kurenai] menu key
        int mStartScancode = 0; // [Kurenai] start key (the pad's Start for the game), 0 = none
        int mActivateScancode = 0; // [Kurenai] activate key (Morrowind's activate), 0 = none
        bool mActivatePrev = false;
        bool mHookAimManual = true; // [Kurenai] hook aim
        bool mMenuOpen = false; // the Lua options menu is up
        std::uint16_t mMenuPrevMask = 0xffff; // the pad buttons seen last frame (0xffff: ignore the first frame)
        bool mMenuChordPrev = false, mJoyMenuNow = false;
        float mNavRepeat = 0;
        std::vector<std::string> mNav;
        float mDeathWait = 0;
        int mDeathState = 0; // 0 alive, 1 her death sequence plays, 2 it is over
        float mTestKillClock = 0;
        bool mTestKillDone = false;
        bool mJoyWeaponNow = false; // a raw joystick's weapon-screen button is down
        std::map<int, _SDL_Joystick*> mRawPads; // joysticks without a gamepad layout, opened by SDL index
        std::vector<std::string> mJoyMap; // the 'joystick map' words, by button number
        std::set<int> mJoySeen; // raw buttons already named in the log (device * 256 + button)
        int mWeaponRow = 0;
        std::array<std::uint8_t, 4> mWeaponLevels{}; // the versions the game has (what the weapon screen last equipped)
        std::array<std::uint8_t, 4> mWeaponBrowse{}; // the versions shown while choosing
        bool mWeaponEquipPrev = false, mWeaponBackPrev = false;
        bool mWeaponWant = false; // the weapon key was pressed with the item menu closed: open it, then focus the weapons
        std::uint64_t mWeaponWantNs = 0;
        int mAttachWant = -1; // the weapon screen asked for this tetsugen head (0 blade, 1 fundo); -1 none
        bool mItemTriPrev = false, mItemCirPrev = false; // Triangle / Circle seen last frame in the item menu
        bool mWeaponPrev[5] = { false, false, false, false, false }; // key, up, down, left, right
        bool mControlsHeld = false;
        bool mToldControls = false;
        bool mCollisionWasOn = false;
        bool mFollowGround = true; // [Kurenai] height mode = ground
        bool mInterpolate = true;
        bool mPredict = true; // carry her on past the newest state instead of interpolating behind it
        osg::Vec3f mGameFeet; // Morrowind position of her newest game state (no prediction)
        float mBlend = 1; // this frame's place between the last two states (above 1: predicted)
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
        int mAttachment = 1; // her tetsugen: 0 blade, 1 fundo
        bool mAttachPrevLeft = false, mAttachPrevRight = false; // the attachment keys' previous state (for the log)
        float mNinjutsuLeft = 0.f; // seconds of ninjutsu slow motion left (Morrowind's simulation scale is lowered)
        std::uint32_t mNinjutsuPrev = 0;
        bool mForcingSneak = false; // we set Flag_ForceSneak (her crouch / wall hug, the anim map's sneak line)
        // Swimming: how far below the game's swimming height she is in Morrowind (diving with the breathing tube;
        // the bridge thread shifts the game's collision queries by it).
        std::atomic<float> mSwimDepth{ 0 };
        std::atomic<bool> mSwimming{ false }; // a swim motion plays, in a cell with water
        float mSwimPitch = 0; // Morrowind's look up/down while she swims (radians, positive looks down)
        std::atomic<float> mWaterLevel{ 0 }; // that cell's water level
        std::atomic<std::uint64_t> mWaterHits{ 0 }, mSwimFloorHits{ 0 };
        void updateSneak(std::uint32_t motion);
        void updateNinjutsu(std::uint32_t motion, float dt);
        std::FILE* mLog = nullptr;

        // The bridge thread owns the incoming ring: it answers CollisionQuery at once and queues the rest for the
        // main thread. Both threads send, so sends are serialised.
        std::thread mThread;
        std::atomic<bool> mStop{ false };
        std::atomic<std::uint64_t> mFrameBeatNs{ 0 }; // the frame's last heartbeat (0: no game running yet)
        std::mutex mSendMutex;
        std::mutex mInboxMutex;
        struct Arrived
        {
            kurenai::proto::Message message;
            std::uint64_t arrivalNs; // when the bridge thread took it off the ring
        };
        std::deque<Arrived> mInbox;
        std::timed_mutex mWorldMutex;
        std::atomic<bool> mWorldStalled{ false }; // a query found the world lock held for long (a cell loading)
        std::unique_lock<std::timed_mutex> queryLock();
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
        std::uint64_t mCombatEvents = 0, mCombatHits = 0, mStealthKills = 0;

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

        // Lock-on (R1): Morrowind picks the target from all living actors; the game turns her toward it.
        bool mLockOn = true;
        float mLockRange = 3000;
        bool mLockHeld = false;
        bool mToldToggle = false;
        MWWorld::Ptr mLockActor;
        MWWorld::Ptr mLockLast; // the last target, kept for a moment: R1 pressed again or flickering re-locks it
        double mLockLastTime = 0;
        std::uint64_t mLockPicks = 0, mLockMisses = 0;
        MyGUI::ImageBox* mLockMarker = nullptr;
        MyGUI::ITexture* mLockTexture = nullptr;
        void updateLockOn(const kurenai::CoordTransform& t);
        MWWorld::Ptr pickLockTarget() const;
        bool lockable(const MWWorld::Ptr& actor) const;
        void updateLockMarker();
        std::uint32_t actorId(const MWWorld::Ptr& actor);
        // Morrowind's radar, drawn into the game HUD's radar region (0 = the game's own)
        int mRadarRegion = 2;
        float mRadarRange = 4000;
        void drawRadar(std::uint32_t* data, int pitch, int w, int h);
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
            bool release = false; // it lives: the body goes back to Morrowind's animation
            std::uint64_t poses = 0;
            int fxDone = 0; // kill effects shown so far (blood, sparks)
        };
        std::map<std::uint32_t, Victim> mVictims; // by proxy id
        // Blood and sparks on a kill (the game draws its own; Morrowind's effect meshes stand in): removed after their life.
        struct KillFx
        {
            std::string id;
            float life = 0;
        };
        std::vector<KillFx> mKillFx;
        int mKillFxCount = 0;
        void spawnKillFx(const MWWorld::Ptr& actor, int stage);
        void updateKillFx(float dt);

        // PS2 animation ([Kurenai] ps2 animation): the game's skeleton, retargeted onto the player's.
        enum class Ps2Anim
        {
            Off,
            Locomotion,
            All
        } mPs2Anim = Ps2Anim::Off;
        std::map<std::uint32_t, kurenai::SkeletonMap> mSkeletonMaps; // by model CRC (0: maps without a model line)
        std::uint32_t mPoseModel = 0xffffffffu; // the model of the last pose (logged when it changes)
        std::map<std::uint32_t, std::uint32_t> mPickedMaps; // model id no map names -> the map its limbs fit
        std::vector<kurenai::proto::Vec3> mPoseBones; // the last pose, Red Ninja world
        // The native body ([Kurenai] native body): her own PS2 model, posed with the last pose's matrices unchanged.
        std::unique_ptr<NativeBody> mNative;
        std::optional<kurenai::proto::Pose> mLatestPose;
        bool mNativeShown = false;

        // Her wire, drawn while the game has it out: from her hand (the game's) to where it is fixed.
        osg::ref_ptr<osg::Geometry> mWireGeometry;
        osg::ref_ptr<osg::Vec3Array> mWireVertices;
        void updateWire(const kurenai::CoordTransform& t);

        // Her thrown kunai (only PCSX2 has it): PCSX2 sends its launch (start and direction, a kQueryProjectile query
        // nothing answers); it flies straight to the first thing in Morrowind along that line and stays a while in a
        // wall or the ground.
        struct KunaiLaunch
        {
            osg::Vec3f from, to; // Morrowind
        };
        std::mutex mKunaiMutex;
        std::vector<KunaiLaunch> mKunaiNew; // from the bridge thread, guarded by mKunaiMutex
        enum class Kunai
        {
            Hidden,
            Flying,
            Stuck
        } mKunai
            = Kunai::Hidden;
        osg::ref_ptr<osg::MatrixTransform> mKunaiNode;
        osg::Matrixf mKunaiMesh; // the mesh's centre to the origin, its long axis onto +x
        osg::Vec3f mKunaiPos, mKunaiEnd, mKunaiDir{ 1, 0, 0 };
        bool mKunaiSticks = false;
        float mKunaiTimer = 0;
        std::uint64_t mKunaiThrows = 0;
        // The actor her lock was last on and how long ago: the game drops the lock before the
        // kunai leaves her hand.
        ESM::RefNum mKunaiTarget;
        float mKunaiTargetAge = 0;
        void updateKunai(const kurenai::CoordTransform& t, float dt);
        bool makeKunai();
        std::shared_ptr<PoseTarget> mPoseTarget = std::make_shared<PoseTarget>();
        osg::ref_ptr<PoseOverride> mPoseCallback;
        osg::observer_ptr<osg::Node> mPoseSkeleton; // the animation object can be rebuilt; never keep it alive
        std::uint64_t mPosesReceived = 0;

        // Her items. Kiton (PlayerState.kiton, the game's invisibility): while it lasts and she stands still or creeps
        // (the anim map's "kiton" and "sneak" lines), she has Chameleon 100, so Morrowind's actors don't see her and
        // she is drawn faint, as the game draws her. The smoke bomb (the anim map's "smoke" motion): a cloud where she
        // threw it, as long as the game's; actors in it are blinded, choke (paralysed) and lose her.
        bool mKitonOn = false;
        void updateKiton(std::uint32_t motion, float kiton);
        // [Kurenai] health: "morrowind" (Morrowind's counts; the game keeps her safe and shows it), "game" (the
        // player's Health follows the game's bar) or "off".
        std::string mHealthMode = "morrowind";
        float mLastGameHealth = -1;
        int mHealSeen = -1; // the game's potion/elixir counter (PlayerState.flags top bits) as last applied
        bool mReanchor = false; // take a new anchor on her next state (the old one still answers until then)
        void updateHealth(float health, float healthMax);
        void applyHeals(std::uint32_t flags);
        void sendMorrowindHealth();
        std::string mSmokeModel;
        float mSmokeRadius = 300;
        std::uint32_t mItemMotion = 0xffffffffu;
        struct Smoke
        {
            std::string effectId;
            osg::Vec3f position;
            float age = 0; // seconds since she started the throw
            bool drawn = false;
        };
        std::vector<Smoke> mSmokes;
        struct Smoked
        {
            MWWorld::Ptr actor;
            float left = 0; // seconds until the smoke's spell comes off
        };
        std::map<const void*, Smoked> mSmoked;
        std::uint64_t mSmokeBombs = 0, mSmokedActors = 0;
        // The Hook (HookTargets): places above and around her (roof edges, branches, ledges) that she can see, sent to
        // PCSX2, which puts the stage's own hook points there. The place her wire hangs from is kept while it is out.
        bool mHooks = true;
        float mSinceHookScan = 1e9f;
        std::vector<kurenai::proto::ProxyTarget> mHookTargets;
        std::map<std::uint32_t, int> mHookMisses; // scans in a row a sent place was not found again (kept up to 4)
        std::uint64_t mHookScans = 0;
        void sendHookTargets(const kurenai::CoordTransform& t, float dt);
        // The Hook's hand aim along the engine's own ray (sendHookTargets): its heading and pitch (radians, Morrowind),
        // and the surface point it meets.
        bool mHookAiming = false;
        float mAimYaw = 0, mAimPitch = 0;
        bool mAimHit = false;
        osg::Vec3f mAimAt;
        // The smoke bomb's throw (FlagSmokeTriangle, FlagSmokeCircle): while the button it is on is held, the game
        // doesn't see the button and a grenade arc shows where the bomb will land (the mouse or the right stick raise and
        // lower it); letting go taps the button, the game throws, and the bomb flies along the arc. Its cloud is where
        // it lands.
        std::uint16_t mSmokeButton = 0; // the held button carrying the bomb, 0: not aiming
        std::uint16_t mSmokeTapButton = 0;
        float mSmokeTap = 0; // seconds the button is still sent for the throw
        float mSmokeElev = 0.5f; // the throw's elevation, radians above level
        float mLookPitch = 0; // this frame's mouse look (the player's pitch before it is put back)
        std::vector<osg::Vec3f> mSmokeArc; // points kSmokeStep seconds apart; the last is where it lands
        osg::ref_ptr<osg::Group> mSmokeAimNode;
        osg::ref_ptr<osg::Geometry> mSmokeArcLine, mSmokeRing;
        osg::ref_ptr<osg::MatrixTransform> mSmokeBombNode;
        std::vector<osg::Vec3f> mSmokeFlight; // the thrown bomb's path, empty: none in flight
        float mSmokeFlightTime = 0;
        void smokeArc(const MWWorld::Ptr& player, std::vector<osg::Vec3f>& arc);
        void updateSmokeAim(const MWWorld::Ptr& player, float dt);
        void startSmoke(const osg::Vec3f& at, bool landed = false);
        void updateSmoke(float dt);
        void clearSmoke();

        // Game camera ([Kurenai] camera mode = game): Red Ninja's camera, converted to Morrowind's world.
        bool mGameCameraMode = false;
        bool mCameraTaken = false;
        // The game's lens ([Kurenai] game lens): Morrowind's field of view and viewing distance while its camera shows.
        bool mGameLens = true;
        float mMaxViewDistance = 0;
        float mSavedFov = 0, mSavedViewDistance = 0, mAppliedFov = 0, mAppliedViewDistance = 0;
        bool mLensTaken = false;
        void applyGameLens();
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
        // [Kurenai] hd ui: the item menu and HUD drawn natively from the game's state (UiState)
        std::unique_ptr<HdUi> mHdUi;
        kurenai::proto::UiState mUi;
        float mSinceUi = 1e9f;
        bool mHdUiTried = false;
        bool mGameMenuFull = false; // the captured game frame is a full-screen menu (the item menu), whatever the hi-res UI does
        bool mHudMenuUp = false; // a full-screen game menu (items, pause) is shown
        void updateHdUi(float dt);
        void updateHud(float dt);
        void hideHud();
        std::atomic<std::uint64_t> mQueries{ 0 }, mQueryHits{ 0 }, mQueriesDeclined{ 0 }, mQueryNs{ 0 };
        std::atomic<std::uint64_t> mQueryLockNs{ 0 }, mQueryLockMaxNs{ 0 }; // time waiting for the world lock
        std::atomic<std::uint64_t> mQueriesStalled{ 0 }; // declined because the world stayed locked
    };
}

#endif
