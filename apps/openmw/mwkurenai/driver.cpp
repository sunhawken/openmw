#include "driver.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

#include <MyGUI_Gui.h>
#include <MyGUI_TextBox.h>

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/damagesourcetype.hpp"
#include "../mwinput/actions.hpp"
#include "../mwmechanics/movement.hpp"
#include "../mwphysics/joltlayers.hpp"
#include "../mwphysics/raycasting.hpp"
#include <components/sceneutil/skeleton.hpp>

#include "../mwrender/animation.hpp"
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
            mAnswerCollision = k.mAnswerCollision;
            mCombat = k.mCombat;
            mGameCameraMode = k.mCameraMode.get() == "game";
            mShowStatus = k.mShowStatus;
            const std::string ps2 = k.mPs2Animation;
            mPs2Anim = ps2 == "all" ? Ps2Anim::All : ps2 == "locomotion" ? Ps2Anim::Locomotion : Ps2Anim::Off;
            if (mPs2Anim != Ps2Anim::Off)
            {
                kurenai::ParseError perr;
                mSkeletonMap = kurenai::loadSkeletonMap(k.mSkeletonMap, perr);
                if (!mSkeletonMap)
                {
                    Log(Debug::Error) << "Kurenai: ps2 animation off: skeleton map '" << k.mSkeletonMap.get()
                                      << "' line " << perr.line << ": " << perr.message;
                    mPs2Anim = Ps2Anim::Off;
                }
            }
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
            std::fprintf(mLog, "frame\tmono_ns\tpeer_frame\tsent_ns\trn_x\trn_y\trn_z\tmw_x\tmw_y\tmw_z\tyaw\tmotion\tarrival_ns\tseq\n");
        Log(Debug::Info) << "Kurenai: bridge region '" << name << "' created (protocol " << kurenai::proto::kMajor
                         << "." << kurenai::proto::kMinor << ", " << animMap().size() << " animation mappings, "
                         << (mAnswerCollision ? "answering" : "not answering")
                         << " collision queries); waiting for PCSX2";
        if (mAnswerCollision && Settings::physics().mAsyncNumThreads > 0)
            Log(Debug::Warning) << "Kurenai: [Physics] async num threads > 0: physics may step outside the world lock "
                                   "while collision queries are answered; set it to 0 for KURENAI sessions";
        mThread = std::thread([this] { serviceThread(); });
    }

    Driver::~Driver()
    {
        if (mLatency.count)
            Log(Debug::Info) << "Kurenai: PlayerState latency PCSX2 -> OpenMW: transport average "
                             << mLatency.transportSumUs / mLatency.count << " us (max " << mLatency.transportMaxUs
                             << "), until applied in a frame average " << mLatency.sumUs / mLatency.count
                             << " us (max " << mLatency.maxUs << ") over " << mLatency.count << " states";
        mStop = true;
        if (mThread.joinable())
            mThread.join();
        if (mCombatEvents)
            Log(Debug::Info) << "Kurenai: combat: " << mCombatEvents << " blade segments, " << mCombatHits
                             << " hits on Morrowind actors";
        if (mQueries)
            Log(Debug::Info) << "Kurenai: answered " << mQueries << " collision queries (" << mQueryHits << " hits, "
                             << mQueriesDeclined << " declined), average " << (mQueryNs / 1000.0 / mQueries)
                             << " us each, of which waiting for the world lock " << (mQueryLockNs / 1000.0 / mQueries)
                             << " us (max " << mQueryLockMaxNs / 1000.0 << " us)";
        if (mLink)
            Log(Debug::Info) << "Kurenai: bridge: received " << mLink->stats().received << ", sent "
                             << mLink->stats().sent << ", send failures " << mLink->stats().sendFailed
                             << ", malformed " << mLink->stats().decodeErrors << ", lost (sequence gaps) "
                             << mLink->stats().seqGaps;
        if (mLog)
            std::fclose(mLog);
    }

    bool Driver::send(const kurenai::proto::Payload& p)
    {
        std::lock_guard<std::mutex> lock(mSendMutex);
        return mLink->send(p, mFrames);
    }

    void Driver::serviceThread()
    {
        std::vector<kurenai::proto::Message> msgs;
        std::uint64_t lastActivity = kurenai::monotonicNs();
        while (!mStop)
        {
            msgs.clear();
            mLink->poll(msgs);
            for (kurenai::proto::Message& m : msgs)
            {
                if (const auto* q = std::get_if<kurenai::proto::CollisionQuery>(&m.payload))
                    answer(*q);
                else
                {
                    std::lock_guard<std::mutex> lock(mInboxMutex);
                    mInbox.push_back({ std::move(m), kurenai::monotonicNs() });
                }
            }
            const std::uint64_t now = kurenai::monotonicNs();
            if (!msgs.empty())
                lastActivity = now;
            // PCSX2 waits inside the game's collision call for these answers: spin while it is busy, rest when idle.
            if (now - lastActivity < 50'000'000)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    void Driver::answer(const kurenai::proto::CollisionQuery& q)
    {
        const std::uint64_t start = kurenai::monotonicNs();
        kurenai::proto::CollisionResult res;
        res.queryId = q.queryId;
        kurenai::CoordTransform t;
        bool anchored;
        {
            std::lock_guard<std::mutex> lock(mTransformMutex);
            t = mTransform;
            anchored = mAnchored;
        }
        if (!mAnswerCollision || !anchored || q.kind != kurenai::proto::QueryKind::Ray)
        {
            res.material = kurenai::proto::kMaterialDeclined;
            ++mQueriesDeclined;
            send(res);
            return;
        }
        const kurenai::proto::Vec3 a = t.position(q.from), b = t.position(q.to);
        MWPhysics::RayCastingResult hit;
        {
            const std::uint64_t lockStart = kurenai::monotonicNs();
            std::lock_guard<std::mutex> lock(mWorldMutex);
            const std::uint64_t waited = kurenai::monotonicNs() - lockStart;
            mQueryLockNs += waited;
            if (waited > mQueryLockMaxNs)
                mQueryLockMaxNs = waited;
            // Statics, terrain and doors: Morrowind's level geometry. Actors (Kurenai herself included) are not
            // level geometry for the game's queries.
            hit = MWBase::Environment::get().getWorld()->getRayCasting()->castRay(osg::Vec3f(a.x, a.y, a.z),
                osg::Vec3f(b.x, b.y, b.z), {}, {},
                MWPhysics::Layers::WORLD | MWPhysics::Layers::HEIGHTMAP | MWPhysics::Layers::DOOR);
        }
        res.hit = hit.mHit;
        if (hit.mHit)
        {
            const osg::Vec3f& p = hit.mHitPos;
            res.point = t.toRedNinja({ p.x(), p.y(), p.z() });
            osg::Vec3f n = hit.mHitNormal;
            if (n.normalize() == 0)
                n = osg::Vec3f(0, 0, 1);
            res.normal = kurenai::CoordTransform::axesInverse({ n.x(), n.y(), n.z() });
            const float len = (osg::Vec3f(b.x, b.y, b.z) - osg::Vec3f(a.x, a.y, a.z)).length();
            res.fraction = len > 0 ? std::fmin(1.0f, (p - osg::Vec3f(a.x, a.y, a.z)).length() / len) : 0.0f;
            ++mQueryHits;
        }
        send(res);
        ++mQueries;
        mQueryNs += kurenai::monotonicNs() - start;
    }

    void Driver::onConnect()
    {
        Log(Debug::Info) << "Kurenai: PCSX2 connected; Red Ninja's code now drives the player";
        {
            std::lock_guard<std::mutex> lock(mTransformMutex);
            mAnchored = false;
        }
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
        mPendingRagdolls.clear();
        releaseCamera();
        osg::ref_ptr<osg::Node> skeleton;
        if (mPoseSkeleton.lock(skeleton) && mPoseCallback)
            skeleton->removeUpdateCallback(mPoseCallback);
        mPoseSkeleton = nullptr;
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
        // screen's heading in Morrowind is minus the camera's yaw (facing = (sin, cos) of the heading).
        float stickUp = forward, stickRight = right;
        if (mLatest && (mLatest->flags & kurenai::proto::FlagCamera))
        {
            const float heading = -MWBase::Environment::get().getWorld()->getCamera()->getYaw();
            const float dx = forward * std::sin(heading) + right * std::cos(heading);
            const float dy = forward * std::cos(heading) - right * std::sin(heading);
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
            if (send(in))
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

        std::deque<Arrived> msgs;
        {
            std::lock_guard<std::mutex> lock(mInboxMutex);
            msgs.swap(mInbox);
        }
        for (const Arrived& a : msgs)
        {
            const kurenai::proto::Message& m = a.message;
            if (const auto* ps = std::get_if<kurenai::proto::PlayerState>(&m.payload))
            {
                mPrev = mLatest;
                mLatest = *ps;
                mSinceLatest = 0;
                ++mStatesReceived;
                mLastPeerFrame = m.header.frameId;
                if (now >= m.header.timestampNs && a.arrivalNs >= m.header.timestampNs)
                {
                    const double us = (now - m.header.timestampNs) / 1000.0;
                    const double transportUs = (a.arrivalNs - m.header.timestampNs) / 1000.0;
                    mLatency.count++;
                    mLatency.sumUs += us;
                    mLatency.maxUs = std::max(mLatency.maxUs, us);
                    mLatency.lastUs = us;
                    mLatency.transportSumUs += transportUs;
                    mLatency.transportMaxUs = std::max(mLatency.transportMaxUs, transportUs);
                }
                if (mLog)
                    std::fprintf(mLog, "%llu\t%llu\t%llu\t%llu\t%.3f\t%.3f\t%.3f\t\t\t\t\t%u\t%llu\t%u\n",
                        static_cast<unsigned long long>(mFrames), static_cast<unsigned long long>(now),
                        static_cast<unsigned long long>(m.header.frameId),
                        static_cast<unsigned long long>(m.header.timestampNs), ps->position.x, ps->position.y,
                        ps->position.z, ps->animId, static_cast<unsigned long long>(a.arrivalNs), m.header.seq);
            }
            else if (const auto* ce = std::get_if<kurenai::proto::CombatEvent>(&m.payload))
                handleCombat(*ce);
            else if (const auto* pose = std::get_if<kurenai::proto::Pose>(&m.payload))
                handlePose(*pose);
            else if (const auto* hello = std::get_if<kurenai::proto::Hello>(&m.payload))
            {
                Log(Debug::Info) << "Kurenai: Hello from " << hello->software << " running " << hello->gameSerial;
                kurenai::proto::Hello reply;
                reply.role = kurenai::proto::Role::Engine;
                reply.software = "OpenMW Kurenai";
                send(reply);
            }
        }

        applyPendingRagdolls();
        if (mShowStatus)
            updateStatus(dt);

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

    namespace
    {
        // Closest distance between segments p0-p1 and q0-q1, and the point on the first.
        float segmentDistance(
            const osg::Vec3f& p0, const osg::Vec3f& p1, const osg::Vec3f& q0, const osg::Vec3f& q1, osg::Vec3f& onP)
        {
            const osg::Vec3f d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
            const float a = d1 * d1, e = d2 * d2, f = d2 * r;
            float s = 0, t = 0;
            if (a < 1e-6f && e < 1e-6f)
                s = t = 0;
            else if (a < 1e-6f)
                t = std::clamp(f / e, 0.0f, 1.0f);
            else
            {
                const float c = d1 * r;
                if (e < 1e-6f)
                    s = std::clamp(-c / a, 0.0f, 1.0f);
                else
                {
                    const float b = d1 * d2, denom = a * e - b * b;
                    s = denom > 1e-6f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
                    t = (b * s + f) / e;
                    if (t < 0)
                    {
                        t = 0;
                        s = std::clamp(-c / a, 0.0f, 1.0f);
                    }
                    else if (t > 1)
                    {
                        t = 1;
                        s = std::clamp((b - c) / a, 0.0f, 1.0f);
                    }
                }
            }
            onP = p0 + d1 * s;
            return (onP - (q0 + d2 * t)).length();
        }
    }

    void Driver::handleCombat(const kurenai::proto::CombatEvent& e)
    {
        ++mCombatEvents;
        if (!mCombat || !mAnchored)
            return;
        if (e.attackId != mLastAttack)
        {
            mLastAttack = e.attackId;
            mHitThisAttack.clear(); // each hit of a combo can strike the same target again
        }
        kurenai::CoordTransform t;
        {
            std::lock_guard<std::mutex> lock(mTransformMutex);
            t = mTransform;
        }
        const kurenai::proto::Vec3 endRn{ e.origin.x + e.direction.x * e.reach, e.origin.y + e.direction.y * e.reach,
            e.origin.z + e.direction.z * e.reach };
        const kurenai::proto::Vec3 a = t.position(e.origin), b = t.position(endRn);
        const osg::Vec3f p0(a.x, a.y, a.z), p1(b.x, b.y, b.z);
        const float radius = e.radius * t.scale;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWWorld::Ptr player = world->getPlayerPtr();
        std::vector<MWWorld::Ptr> actors;
        MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
            (p0 + p1) * 0.5f, (p1 - p0).length() * 0.5f + radius + 200.0f, actors);
        if (e.attackId != mLoggedAttack)
        {
            // One diagnostic line per attack: where the blade is and the closest actor.
            mLoggedAttack = e.attackId;
            std::vector<MWWorld::Ptr> near;
            MWBase::Environment::get().getMechanicsManager()->getActorsInRange(p0, 2000.0f, near);
            float best = 1e9f;
            std::string who = "none";
            for (const MWWorld::Ptr& n : near)
                if (n != player)
                {
                    const float d = (n.getRefData().getPosition().asVec3() - p0).length();
                    if (d < best)
                        best = d, who = n.getCellRef().getRefId().toDebugString();
                }
            const osg::Vec3f pp = player.getRefData().getPosition().asVec3();
            Log(Debug::Info) << "Kurenai: attack " << e.attackId << " motion " << (e.kind & kurenai::kCombatMotionMask)
                             << " blade at (" << p0.x() << ", " << p0.y() << ", " << p0.z() << "), player at ("
                             << pp.x() << ", " << pp.y() << ", " << pp.z() << "), nearest actor " << who << " at "
                             << best;
        }
        for (const MWWorld::Ptr& actor : actors)
        {
            if (actor == player || actor.getClass().getCreatureStats(actor).isDead())
                continue;
            if (std::find(mHitThisAttack.begin(), mHitThisAttack.end(), actor.getBase()) != mHitThisAttack.end())
                continue;
            // The actor as an upright capsule: its collision half extents around its feet position.
            const osg::Vec3f half = world->getHalfExtents(actor);
            const osg::Vec3f feet = actor.getRefData().getPosition().asVec3();
            osg::Vec3f hitPos;
            const float d = segmentDistance(p0, p1, feet, feet + osg::Vec3f(0, 0, 2 * half.z()), hitPos);
            if (d > radius + std::max(half.x(), half.y()))
                continue;
            mHitThisAttack.push_back(actor.getBase());
            ++mCombatHits;
            // Morrowind's own melee pipeline (Lua combat): damage, hit reaction, AI aggression, sounds.
            MWBase::Environment::get().getLuaManager()->onHit(player, actor, MWWorld::Ptr(), MWWorld::Ptr(), 0, 1.0f,
                e.damage, true, hitPos, true, MWMechanics::DamageSourceType::Melee);
            osg::Vec3f dir = p1 - p0;
            dir.z() = 0;
            if (dir.normalize() == 0)
                dir = feet - player.getRefData().getPosition().asVec3();
            dir.normalize();
            const osg::Vec3f impulse = dir * 800.0f + osg::Vec3f(0, 0, 200.0f);
            if (e.kind & kurenai::kCombatKnockdown)
                actor.getClass().getCreatureStats(actor).setKnockedDown(true);
            mPendingRagdolls.push_back({ actor, impulse, 60 });

            kurenai::proto::WorldContact c;
            c.sourceId = e.attackId;
            c.targetRef = actor.getCellRef().getRefNum().mIndex;
            c.point = t.toRedNinja({ hitPos.x(), hitPos.y(), hitPos.z() });
            c.impulse = kurenai::CoordTransform::axesInverse({ impulse.x(), impulse.y(), impulse.z() });
            c.flags = (e.kind & kurenai::kCombatKnockdown) ? 1 : 0;
            send(c);
            Log(Debug::Info) << "Kurenai: attack " << e.attackId << " (motion " << (e.kind & kurenai::kCombatMotionMask)
                             << ") hits " << actor.getCellRef().getRefId() << " for " << e.damage
                             << " damage (health before " << actor.getClass().getCreatureStats(actor).getHealth().getCurrent()
                             << ")";
        }
    }

    void Driver::applyPendingRagdolls()
    {
        // A killing blow's ragdoll starts with the blade's push, in the window after Lua applied the damage and
        // before the death handling would start it with its default impulse.
        MWBase::World* world = MWBase::Environment::get().getWorld();
        for (auto it = mPendingRagdolls.begin(); it != mPendingRagdolls.end();)
        {
            const MWWorld::Ptr& actor = it->actor;
            bool done = --it->framesLeft <= 0 || !actor.getRefData().isEnabled();
            if (!done && actor.getClass().getCreatureStats(actor).isDead())
            {
                if (!world->hasRagdoll(actor))
                {
                    world->activateActorRagdoll(actor, it->impulse);
                    Log(Debug::Info) << "Kurenai: killing blow on " << actor.getCellRef().getRefId()
                                     << (world->hasRagdoll(actor) ? ": ragdoll pushed along the blade ("
                                                                  : ": no ragdoll (disabled or no skeleton), impulse (")
                                     << it->impulse.x() << ", " << it->impulse.y() << ", " << it->impulse.z() << ")";
                }
                done = true;
            }
            it = done ? mPendingRagdolls.erase(it) : it + 1;
        }
    }

    void Driver::handlePose(const kurenai::proto::Pose& p)
    {
        ++mPosesReceived;
        if (mPs2Anim == Ps2Anim::Off || !mSkeletonMap || !mLatest)
            return;
        const std::vector<kurenai::proto::Vec3> dirs = kurenai::aimDirections(p.bones, mLatest->rotation, *mSkeletonMap);
        auto target = std::make_shared<PoseTarget>();
        const auto& loco = mSkeletonMap->locomotion;
        target->active = mPs2Anim == Ps2Anim::All
            || std::find(loco.begin(), loco.end(), mLatest->animId) != loco.end();
        for (std::size_t i = 0; i < dirs.size(); ++i)
            target->aims.push_back({ mSkeletonMap->aims[i].bone, mSkeletonMap->aims[i].child,
                osg::Vec3f(dirs[i].x, dirs[i].y, dirs[i].z) });
        mPoseTarget = target;
        if (mPoseCallback)
            mPoseCallback->setTarget(mPoseTarget);
    }

    void Driver::updateStatus(float dt)
    {
        mSinceStatus += dt;
        if (mStatus && mSinceStatus < 0.25f)
            return;
        mSinceStatus = 0;
        if (!mStatus)
        {
            mStatus = MyGUI::Gui::getInstance().createWidget<MyGUI::TextBox>(
                "SandText", MyGUI::IntCoord(8, 8, 900, 24), MyGUI::Align::Default, "Debug");
            mStatus->setNeedMouseFocus(false);
        }
        std::ostringstream s;
        s.precision(1);
        s << std::fixed << "KURENAI ";
        if (!mPeerAlive)
            s << "waiting for PCSX2";
        else
        {
            s << "PCSX2 frame " << mLastPeerFrame << "  latency " << mLatency.lastUs / 1000.0 << " ms (avg "
              << (mLatency.count ? mLatency.sumUs / mLatency.count / 1000.0 : 0.0) << ")";
            if (mLatest)
                s << "  motion " << mLatest->animId;
            if (mQueries)
                s << "  collision " << mQueries.load() << " (" << (mQueryNs / 1000.0 / mQueries) << " us)";
            if (mCombatHits)
                s << "  hits " << mCombatHits;
            if (mPs2Anim != Ps2Anim::Off)
                s << "  ps2 pose " << (mPoseTarget->active ? "on" : "off");
        }
        mStatus->setCaption(s.str());
    }

    void Driver::applyGameCamera()
    {
        if (!(mLatest->flags & kurenai::proto::FlagCamera))
            return;
        kurenai::CoordTransform t;
        {
            std::lock_guard<std::mutex> lock(mTransformMutex);
            t = mTransform;
        }
        const kurenai::proto::Vec3 eye = t.position(mLatest->cameraEye);
        const kurenai::proto::Vec3 target = t.position(mLatest->cameraTarget);
        osg::Vec3f dir(target.x - eye.x, target.y - eye.y, target.z - eye.z);
        const float len = dir.normalize();
        if (len < 1e-3f)
            return;
        MWRender::Camera* camera = MWBase::Environment::get().getWorld()->getCamera();
        if (!mCameraTaken)
        {
            mPreviousCameraMode = static_cast<int>(camera->getMode());
            camera->setMode(MWRender::Camera::Mode::Static, true);
            mCameraTaken = true;
            Log(Debug::Info) << "Kurenai: showing Red Ninja's camera";
        }
        // Morrowind's camera looks along (-sin yaw cos pitch, cos yaw cos pitch, sin pitch): its yaw is minus an
        // actor's heading (Camera::rotateCameraToTrackingPtr), and a positive pitch looks up.
        camera->setStaticPosition(osg::Vec3d(eye.x, eye.y, eye.z));
        camera->setYaw(-std::atan2(dir.x(), dir.y()), true);
        camera->setPitch(std::asin(std::clamp(dir.z(), -1.0f, 1.0f)), true);
    }

    void Driver::releaseCamera()
    {
        if (!mCameraTaken)
            return;
        MWBase::Environment::get().getWorld()->getCamera()->setMode(
            static_cast<MWRender::Camera::Mode>(mPreviousCameraMode), true);
        mCameraTaken = false;
    }

    void Driver::updatePoseOverride()
    {
        if (mPs2Anim == Ps2Anim::Off)
            return;
        // The player's third-person skeleton; the animation object is rebuilt on some changes, so re-attach.
        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWRender::Animation* anim = world->getAnimation(world->getPlayerPtr());
        osg::Node* skeleton = anim ? anim->getSkeleton() : nullptr;
        osg::ref_ptr<osg::Node> current;
        if (mPoseSkeleton.lock(current) && current.get() == skeleton)
            return;
        if (current && mPoseCallback)
            current->removeUpdateCallback(mPoseCallback);
        mPoseSkeleton = skeleton;
        if (!skeleton)
            return;
        mPoseCallback = new PoseOverride(mPoseTarget);
        skeleton->addUpdateCallback(mPoseCallback);
        Log(Debug::Info) << "Kurenai: PS2 animation (" << (mPs2Anim == Ps2Anim::All ? "all" : "locomotion")
                         << ") attached to the player's skeleton, " << mSkeletonMap->aims.size() << " bones";
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
            {
                std::lock_guard<std::mutex> lock(mTransformMutex);
                mTransform.anchorRn = mLatest->position;
                mTransform.originMw = { p.x(), p.y(), p.z() };
                mAnchored = true;
            }
            mRnGroundY = mLatest->position.y;
            mMwZ = p.z();
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
        updatePoseOverride();
        if (mGameCameraMode)
            applyGameCamera();
        if (mLog)
            std::fprintf(mLog, "%llu\t%llu\t\t\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.4f\t%u\n",
                static_cast<unsigned long long>(mFrames), static_cast<unsigned long long>(kurenai::monotonicNs()),
                rn.x, rn.y, rn.z, mw.x, mw.y, mw.z, yaw, mLatest->animId);
    }
}
