#include "driver.hpp"

#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwinput/actions.hpp"
#include "../mwmechanics/movement.hpp"
#include "../mwphysics/raycasting.hpp"
#include "../mwrender/camera.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

namespace MWKurenai
{
    namespace
    {
        constexpr float kGameHz = 30.0f;

        // Red Ninja motion id -> Morrowind animation group, from the profile's anim_map.tsv ("motion<TAB>group").
        std::map<std::uint32_t, std::string> loadAnimMap(const std::string& path)
        {
            std::map<std::uint32_t, std::string> map;
            if (path.empty())
                return map;
            std::ifstream in(path);
            if (!in)
            {
                Log(Debug::Warning) << "Kurenai: cannot read animation map " << path;
                return map;
            }
            std::string line;
            while (std::getline(in, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;
                std::istringstream fields(line);
                std::uint32_t motion = 0;
                std::string group;
                if (fields >> motion >> group)
                    map[motion] = group;
            }
            return map;
        }

        std::map<std::uint32_t, std::string>& animMap()
        {
            static std::map<std::uint32_t, std::string> map;
            return map;
        }

        kurenai::proto::Vec3 lerp(const kurenai::proto::Vec3& a, const kurenai::proto::Vec3& b, float t)
        {
            return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
        }

        float lerpAngle(float a, float b, float t)
        {
            float d = std::remainder(b - a, 2.0f * static_cast<float>(M_PI));
            return a + d * t;
        }

        std::uint8_t axisByte(float v) // -1..1 -> 0..255, 128 = centre
        {
            const float c = std::fmax(-1.0f, std::fmin(1.0f, v));
            return static_cast<std::uint8_t>(std::lround(128.0f + c * 127.0f));
        }
    }

    Driver::Driver()
    {
        std::string name, logPath;
        try
        {
            const Settings::KurenaiCategory& k = Settings::kurenai();
            if (!k.mEnabled)
                return;
            name = k.mBridgeName;
            mTransform.scale = k.mScale;
            mFollowGround = k.mHeightMode.get() != "game";
            animMap() = loadAnimMap(k.mAnimationMap);
            logPath = k.mStateLog;
            mPeerTimeoutNs = static_cast<std::uint64_t>(k.mPeerTimeout * 1e9f);
            mInterpolate = k.mInterpolate;
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "Kurenai: disabled, bad [Kurenai] settings: " << e.what();
            return;
        }
        std::string error;
        mLink = kurenai::Link::open(name, kurenai::SharedRegion::Side::Engine, error);
        if (!mLink)
        {
            Log(Debug::Error) << "Kurenai: cannot create bridge region '" << name << "': " << error;
            return;
        }
        if (!logPath.empty() && (mLog = std::fopen(logPath.c_str(), "w")))
            std::fprintf(mLog, "frame\tmono_ns\tpeer_frame\tsent_ns\trn_x\trn_y\trn_z\tmw_x\tmw_y\tmw_z\tyaw\tmotion\n");
        Log(Debug::Info) << "Kurenai: bridge region '" << name << "' created (protocol " << kurenai::proto::kMajor
                         << "." << kurenai::proto::kMinor << ", " << animMap().size()
                         << " animation mappings); waiting for PCSX2";
    }

    Driver::~Driver()
    {
        if (mLog)
            std::fclose(mLog);
    }

    void Driver::onConnect()
    {
        Log(Debug::Info) << "Kurenai: PCSX2 connected; Red Ninja's code now drives the player";
        mAnchored = false;
        mPrev.reset();
        mLatest.reset();
        mLastMotion = 0xffffffffu;
        // The game's own code integrates Kurenai's motion; Morrowind's gravity and collision must not fight it
        // (Phase 3 answers the game's collision queries from Morrowind instead).
        MWBase::World* world = MWBase::Environment::get().getWorld();
        // toggleCollisionMode returns the new state.
        const bool nowOn = world->toggleCollisionMode();
        if (nowOn)
            world->toggleCollisionMode(); // it was already off (tcl): leave it off, and don't restore it later
        mCollisionWasOn = !nowOn;
    }

    void Driver::onDisconnect()
    {
        Log(Debug::Warning) << "Kurenai: PCSX2 stopped responding; Morrowind controls the player again";
        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (mCollisionWasOn && !world->toggleCollisionMode())
            world->toggleCollisionMode();
        mCollisionWasOn = false;
        MWWorld::Ptr player = world->getPlayerPtr();
        MWBase::Environment::get().getMechanicsManager()->playAnimationGroup(player, "idle", 0, 1, false);
        mLastMotion = 0xffffffffu;
    }

    void Driver::sendInput()
    {
        const MWBase::InputManager* input = MWBase::Environment::get().getInputManager();
        const float forward
            = input->getActionValue(MWInput::A_MoveForward) - input->getActionValue(MWInput::A_MoveBackward);
        const float right = input->getActionValue(MWInput::A_MoveRight) - input->getActionValue(MWInput::A_MoveLeft);

        // Turn "forward/right on screen" into Red Ninja's stick, which is relative to the game's camera. The
        // screen's forward in Morrowind is the camera's yaw (facing = (sin, cos)).
        float stickUp = forward, stickRight = right;
        if (mLatest && (mLatest->flags & kurenai::proto::FlagCamera))
        {
            const float psi = MWBase::Environment::get().getWorld()->getCamera()->getYaw();
            const float dx = forward * std::sin(psi) + right * std::cos(psi);
            const float dy = forward * std::cos(psi) - right * std::sin(psi);
            // Morrowind (x, y) on the ground is Red Ninja (x, -z).
            const float rx = dx, rz = -dy;
            const float fx = mLatest->cameraTarget.x - mLatest->cameraEye.x;
            const float fz = mLatest->cameraTarget.z - mLatest->cameraEye.z;
            const float len = std::hypot(fx, fz);
            if (len > 1e-3f)
            {
                const float ux = fx / len, uz = fz / len;
                stickUp = rx * ux + rz * uz;
                stickRight = rx * -uz + rz * ux; // the game's screen-right is forward x up = (-fz, fx)
            }
        }

        kurenai::proto::Input in;
        in.lx = axisByte(stickRight);
        in.ly = axisByte(-stickUp); // the pad's Y grows downwards
        // Button mapping (Morrowind action -> DualShock 2). Red Ninja: Cross jump, Square attack, Circle wire,
        // Triangle action, R1 lock-on, L1 crouch/stealth. Needs proper testing against the game's manual.
        const auto held = [&](int action) { return input->getActionValue(action) > 0.5f; };
        if (held(MWInput::A_Jump))
            in.buttons |= kurenai::proto::BtnCross;
        if (held(MWInput::A_Use))
            in.buttons |= kurenai::proto::BtnSquare;
        if (held(MWInput::A_Activate))
            in.buttons |= kurenai::proto::BtnCircle;
        if (held(MWInput::A_ToggleWeapon))
            in.buttons |= kurenai::proto::BtnTriangle;
        if (held(MWInput::A_Sneak))
            in.buttons |= kurenai::proto::BtnL1;
        if (held(MWInput::A_Run))
            in.buttons |= kurenai::proto::BtnR1;
        if (held(MWInput::A_GameMenu))
            in.buttons |= kurenai::proto::BtnStart;

        // Send on change, and at least 4 times a second so a lost message can't leave a button stuck.
        if (!(in == mLastInput) || mSinceInput > 0.25f)
        {
            if (mLink->send(in, mFrames))
            {
                mLastInput = in;
                mSinceInput = 0;
            }
        }
    }

    void Driver::beforeMechanics(float dt)
    {
        if (!mLink)
            return;
        ++mFrames;
        mSinceInput += dt;
        mSinceLatest += dt;
        const std::uint64_t now = kurenai::monotonicNs();
        mLink->tick(now);

        std::vector<kurenai::proto::Message> msgs;
        mLink->poll(msgs);
        for (const kurenai::proto::Message& m : msgs)
        {
            if (const auto* ps = std::get_if<kurenai::proto::PlayerState>(&m.payload))
            {
                mPrev = mLatest;
                mLatest = *ps;
                mSinceLatest = 0;
                ++mStatesReceived;
                if (mLog)
                    std::fprintf(mLog, "%llu\t%llu\t%llu\t%llu\t%.3f\t%.3f\t%.3f\t\t\t\t\t%u\n",
                        static_cast<unsigned long long>(mFrames), static_cast<unsigned long long>(now),
                        static_cast<unsigned long long>(m.header.frameId),
                        static_cast<unsigned long long>(m.header.timestampNs), ps->position.x, ps->position.y,
                        ps->position.z, ps->animId);
            }
            else if (const auto* hello = std::get_if<kurenai::proto::Hello>(&m.payload))
            {
                Log(Debug::Info) << "Kurenai: Hello from " << hello->software << " running " << hello->gameSerial;
                kurenai::proto::Hello reply;
                reply.role = kurenai::proto::Role::Engine;
                reply.software = "OpenMW Kurenai";
                mLink->send(reply, mFrames);
            }
        }

        const bool alive = mLink->peerAlive(now, mPeerTimeoutNs);
        if (alive && !mPeerAlive)
            onConnect();
        else if (!alive && mPeerAlive)
            onDisconnect();
        mPeerAlive = alive;
        if (!mPeerAlive)
            return;

        sendInput();

        // Morrowind's own movement for the player is switched off while the game drives her.
        MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (auto* controls = MWBase::Environment::get().getLuaManager()->getActorControls(player))
        {
            controls->mMovement = 0;
            controls->mSideMovement = 0;
            controls->mJump = false;
            controls->mUse = MWMechanics::AttackType::NoAttack;
        }
        player.getClass().getMovementSettings(player).mPosition[0] = 0;
        player.getClass().getMovementSettings(player).mPosition[1] = 0;
        player.getClass().getMovementSettings(player).mPosition[2] = 0;
    }

    void Driver::applyAnimation(std::uint32_t motion)
    {
        if (motion == mLastMotion)
            return;
        mLastMotion = motion;
        const auto it = animMap().find(motion);
        if (it == animMap().end())
            return; // unmapped: keep what is playing
        MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        // Mode 2: start now and loop; scripted, so the character controller does not replace it.
        MWBase::Environment::get().getMechanicsManager()->playAnimationGroup(player, it->second, 2, 1000000, true);
    }

    void Driver::afterPhysics(float dt)
    {
        (void)dt;
        if (!mLink || !mPeerAlive || !mLatest)
            return;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWWorld::Ptr player = world->getPlayerPtr();
        if (!mAnchored)
        {
            // Kurenai starts where the player stands; from then on her motion is the game's.
            const osg::Vec3f p = player.getRefData().getPosition().asVec3();
            mTransform.anchorRn = mLatest->position;
            mTransform.originMw = { p.x(), p.y(), p.z() };
            mRnGroundY = mLatest->position.y;
            mMwZ = p.z();
            mAnchored = true;
            Log(Debug::Info) << "Kurenai: anchored Red Ninja (" << mLatest->position.x << ", " << mLatest->position.y
                             << ", " << mLatest->position.z << ") at Morrowind (" << p.x() << ", " << p.y() << ", "
                             << p.z() << "), scale " << mTransform.scale;
        }

        // Interpolate between the game's 30 Hz states (adds up to one game frame of delay, measured in Phase 6).
        const float t = (mPrev && mInterpolate) ? std::fmin(1.0f, mSinceLatest * kGameHz) : 1.0f;
        const kurenai::proto::PlayerState& a = mPrev ? *mPrev : *mLatest;
        const kurenai::proto::Vec3 rn = lerp(a.position, mLatest->position, t);
        kurenai::proto::Vec3 mw = mTransform.position(rn);
        if (mFollowGround)
        {
            // Until the game's own collision queries are answered from Morrowind (Phase 3), Morrowind supplies the
            // ground: Kurenai stands on whatever is below her, at the height the game gives her above its own
            // ground (jumps, falls). Her ground contact is where the game's physics velocity is zero.
            if (!(mLatest->flags & kurenai::proto::FlagAirborne))
                mRnGroundY = mLatest->position.y;
            const float above = std::fmax(0.0f, rn.y - mRnGroundY) * mTransform.scale;
            const float top = mMwZ + 300.0f; // steps and slopes up to here
            const MWPhysics::RayCastingResult hit = world->getRayCasting()->castRay(
                osg::Vec3f(mw.x, mw.y, top), osg::Vec3f(mw.x, mw.y, top - 4000.0f), { player }, {});
            const float ground = hit.mHit ? hit.mHitPos.z() : mMwZ - above;
            mw.z = ground + above;
            mMwZ = ground;
        }
        const float yaw = lerpAngle(mTransform.yaw(a.rotation, { 0, 0, 1 }),
            mTransform.yaw(mLatest->rotation, { 0, 0, 1 }), t);

        world->moveObject(player, osg::Vec3f(mw.x, mw.y, mw.z), true, true);
        player = world->getPlayerPtr();
        world->rotateObject(player, osg::Vec3f(0, 0, yaw), MWBase::RotationFlag_none);
        applyAnimation(mLatest->animId);
        if (mLog)
            std::fprintf(mLog, "%llu\t%llu\t\t\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.4f\t%u\n",
                static_cast<unsigned long long>(mFrames), static_cast<unsigned long long>(kurenai::monotonicNs()),
                rn.x, rn.y, rn.z, mw.x, mw.y, mw.z, yaw, mLatest->animId);
    }
}
