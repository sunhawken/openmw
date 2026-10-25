#include "driver.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ITexture.h>
#include <MyGUI_TextBox.h>

#include <SDL_keyboard.h>

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/damagesourcetype.hpp"
#include "../mwmechanics/drawstate.hpp"
#include "../mwinput/actions.hpp"
#include "../mwmechanics/movement.hpp"
#include "../mwphysics/joltlayers.hpp"
#include "../mwphysics/raycasting.hpp"
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/visitor.hpp>

#include "../mwrender/animation.hpp"
#include "../mwrender/camera.hpp"
#include "../mwrender/renderingmanager.hpp"
#include "../mwrender/vismask.hpp"

#include <components/sceneutil/lightmanager.hpp>

#include <osg/Geometry>
#include <osg/LineWidth>
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwmechanics/spells.hpp"

#include <components/esm/defs.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadspel.hpp>

namespace MWKurenai
{
    namespace
    {
        constexpr float kGameHz = 30.0f;

        // Her motions that are Morrowind's sneak (crouch, creep, wall hug): the anim map's "sneak<TAB>id id ..." lines.
        std::set<std::uint32_t>& sneakMotions()
        {
            static std::set<std::uint32_t> motions;
            return motions;
        }

        // Her motions in which Kiton hides her besides the sneak ones ("kiton<TAB>id id ..."): standing still.
        std::set<std::uint32_t>& kitonMotions()
        {
            static std::set<std::uint32_t> motions;
            return motions;
        }

        // Her smoke bomb throw ("smoke<TAB>id").
        std::set<std::uint32_t>& smokeMotions()
        {
            static std::set<std::uint32_t> motions;
            return motions;
        }

        // The game's smoke cloud (runs/sm1): it shows 50 vsyncs after the throw starts and is gone 330 vsyncs later.
        // Actors stay blind and choking for a while after they leave it.
        constexpr float kSmokeDelay = 50.0f / 60.0f;
        constexpr float kSmokeLife = 330.0f / 60.0f;
        constexpr float kSmokeAfter = 3.0f;

        // A spell of hers, made once and kept in the store (the save keeps it with whoever carries it).
        const ESM::Spell* itemSpell(std::string_view id, std::string_view name, ESM::Spell::SpellType type,
            std::initializer_list<std::pair<ESM::RefId, int>> effects)
        {
            MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            const ESM::RefId refId = ESM::RefId::stringRefId(id);
            if (const ESM::Spell* existing = store.get<ESM::Spell>().search(refId))
                return existing;
            ESM::Spell spell;
            spell.blank();
            spell.mId = refId;
            spell.mName = name;
            spell.mData.mType = type;
            for (const auto& [effect, magnitude] : effects)
            {
                ESM::IndexedENAMstruct e{};
                e.mData.mEffectID = effect;
                e.mData.mRange = ESM::RT_Self;
                e.mData.mMagnMin = e.mData.mMagnMax = magnitude;
                e.mIndex = static_cast<std::uint32_t>(spell.mEffects.mList.size());
                spell.mEffects.mList.push_back(e);
            }
            return store.overrideRecord(spell);
        }

        const ESM::Spell* kitonSpell()
        {
            return itemSpell("kurenai_kiton", "Kiton", ESM::Spell::ST_Ability, { { ESM::MagicEffect::Chameleon, 100 } });
        }

        const ESM::Spell* smokeSpell()
        {
            return itemSpell("kurenai_smoke", "Smoke Bomb", ESM::Spell::ST_Curse,
                { { ESM::MagicEffect::Blind, 100 }, { ESM::MagicEffect::Paralyze, 1 } });
        }

        // Red Ninja motion id -> Morrowind animation group, from the profile's anim_map.tsv ("motion<TAB>group").
        std::map<std::uint32_t, std::string> loadAnimMap(const std::string& path)
        {
            std::map<std::uint32_t, std::string> map;
            sneakMotions().clear();
            kitonMotions().clear();
            smokeMotions().clear();
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
                if (line.rfind("sneak", 0) == 0 || line.rfind("kiton", 0) == 0 || line.rfind("smoke", 0) == 0)
                {
                    std::string word;
                    fields >> word;
                    std::set<std::uint32_t>& set
                        = word == "sneak" ? sneakMotions() : word == "kiton" ? kitonMotions() : smokeMotions();
                    for (std::uint32_t id = 0; fields >> id;)
                        set.insert(id);
                    continue;
                }
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
            mHideMorrowindHud = k.mHideMorrowindHud;
            mHudScale = k.mGameHudScale;
            mProxyEnabled = k.mProxies;
            mProxyRadius = k.mProxyRadius;
            mSmokeModel = k.mSmokeModel;
            mHooks = k.mHookTargets;
            mSmokeRadius = k.mSmokeRadius;
            mMorrowindMode = k.mStartMode.get() == "morrowind";
            if (!k.mModeToggleKey.get().empty())
            {
                mToggleScancode = SDL_GetScancodeFromName(k.mModeToggleKey.get().c_str());
                if (mToggleScancode == SDL_SCANCODE_UNKNOWN)
                    Log(Debug::Error) << "Kurenai: unknown mode toggle key '" << k.mModeToggleKey.get() << "'";
            }
            const std::string ps2 = k.mPs2Animation;
            mPs2Anim = ps2 == "all" ? Ps2Anim::All : ps2 == "locomotion" ? Ps2Anim::Locomotion : Ps2Anim::Off;
            if (mPs2Anim != Ps2Anim::Off)
            {
                std::vector<std::string> errors;
                mSkeletonMaps = kurenai::loadSkeletonMaps(k.mSkeletonMap, errors);
                for (const std::string& e : errors)
                    Log(Debug::Error) << "Kurenai: skeleton map " << e;
                if (mSkeletonMaps.empty())
                {
                    Log(Debug::Error) << "Kurenai: ps2 animation off: no skeleton maps in '" << k.mSkeletonMap.get()
                                      << "'";
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
        if (Settings::kurenai().mGameHud)
        {
            mHud = kurenai::HudSurface::create(name, error);
            if (!mHud)
                Log(Debug::Error) << "Kurenai: no game HUD, cannot create '" << name << "-hud': " << error;
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
        if (mHudFrames)
            Log(Debug::Info) << "Kurenai: game HUD: " << mHudFrames << " frames shown";
        if (mLatency.count)
            Log(Debug::Info) << "Kurenai: PlayerState latency PCSX2 -> OpenMW: transport average "
                             << mLatency.transportSumUs / mLatency.count << " us (max " << mLatency.transportMaxUs
                             << "), until applied in a frame average " << mLatency.sumUs / mLatency.count
                             << " us (max " << mLatency.maxUs << ") over " << mLatency.count << " states";
        mStop = true;
        if (mThread.joinable())
            mThread.join();
        if (mProxyDamage)
            Log(Debug::Info) << "Kurenai: proxies: " << mProxyDamage << " hits passed on, " << mProxyKills << " kills";
        if (mSmokeBombs)
            Log(Debug::Info) << "Kurenai: smoke bombs: " << mSmokeBombs << ", " << mSmokedActors << " actors caught";
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

    void Driver::takeControl()
    {
        Log(Debug::Info) << "Kurenai: combat mode: Red Ninja's code now drives the player";
        {
            std::lock_guard<std::mutex> lock(mTransformMutex);
            mAnchored = false;
        }
        // A fresh anchor: Red Ninja's world starts again where she stands in Morrowind (she may have walked on in
        // Morrowind mode).
        mPrev.reset();
        mLatest.reset();
        mLastMotion = 0xffffffffu;
        // The game's own code integrates Kurenai's motion; Morrowind's gravity and collision must not fight it
        // (Phase 3 answers the game's collision queries from Morrowind instead).
        MWBase::World* world = MWBase::Environment::get().getWorld();
        // Her weapon in combat mode is the game's (the tetsugen): put Morrowind's weapon or spell away.
        MWWorld::Ptr player = world->getPlayerPtr();
        player.getClass().getCreatureStats(player).setDrawState(MWMechanics::DrawState::Nothing);
        // toggleCollisionMode returns the new state.
        const bool nowOn = world->toggleCollisionMode();
        if (nowOn)
            world->toggleCollisionMode(); // it was already off (tcl): leave it off, and don't restore it later
        mCollisionWasOn = !nowOn;
    }

    void Driver::releaseControl(const char* why)
    {
        Log(Debug::Info) << "Kurenai: " << why << "; Morrowind controls the player";
        mPendingRagdolls.clear();
        hideHud();
        if (mWireGeometry)
            mWireGeometry->setNodeMask(0u);
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
        // Combat mode plays her motions as scripted, looping animation groups, which lock Morrowind's character
        // controller (no walking, no weapon): drop them so Morrowind animates her again.
        MWBase::Environment::get().getMechanicsManager()->clearAnimationQueue(player, true);
        mLastMotion = 0xffffffffu;
        updateSneak(0xffffffffu);
        updateKiton(0xffffffffu, 0);
        clearSmoke();
    }

    void Driver::updateSneak(std::uint32_t motion)
    {
        // Her crouch is Morrowind's sneak (detection, the sneak icon, sneak attacks). Flag_ForceSneak, as the ForceSneak
        // script function: the sneak key's own flag is rewritten from the keyboard every frame by playercontrols.lua.
        const bool sneak = sneakMotions().count(motion) != 0;
        if (sneak == mForcingSneak)
            return;
        mForcingSneak = sneak;
        Log(Debug::Info) << "Kurenai: sneak " << (sneak ? "on" : "off") << " (motion " << motion << ")";
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        player.getClass().getCreatureStats(player).setMovementFlag(
            MWMechanics::CreatureStats::Flag_ForceSneak, sneak);
    }

    void Driver::updateKiton(std::uint32_t motion, float kiton)
    {
        // The game fades her out while Kiton lasts and she stands or creeps, and shows her again while she runs,
        // jumps or attacks (runs/kt1-3).
        const bool on = kiton > 0 && (kitonMotions().count(motion) != 0 || sneakMotions().count(motion) != 0);
        if (on == mKitonOn)
            return;
        mKitonOn = on;
        Log(Debug::Info) << "Kurenai: kiton " << (on ? "on" : "off") << " (motion " << motion << ", " << kiton
                         << " left)";
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        MWMechanics::Spells& spells = player.getClass().getCreatureStats(player).getSpells();
        if (on)
            spells.add(kitonSpell());
        else if (spells.hasSpell(kitonSpell()))
            spells.remove(kitonSpell());
    }

    void Driver::sendHookTargets(const kurenai::CoordTransform& t, float dt)
    {
        // The game's lock-on takes hook points up to 1500 of its units away (0x0021df00).
        constexpr float kGameReach = 1400.0f;
        constexpr float kMinAbove = 220.0f; // Morrowind units above her feet: a hook is for going up or across
        constexpr std::size_t kMaxTargets = 8;
        mSinceHookScan += dt;
        if (mSinceHookScan >= 0.25f)
        {
            mSinceHookScan = 0;
            ++mHookScans;
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::Ptr player = world->getPlayerPtr();
            const osg::Vec3f feet = player.getRefData().getPosition().asVec3();
            const osg::Vec3f head = feet + osg::Vec3f(0, 0, 120.0f);
            const float reach = kGameReach * t.scale;
            // Only the world itself: actors walking by must not make places come and go.
            constexpr int kStatic = MWPhysics::Layers::WORLD | MWPhysics::Layers::HEIGHTMAP;
            std::vector<std::pair<float, kurenai::proto::ProxyTarget>> found;
            int columns = 0, tops = 0;
            // Columns on three rings around her: the highest surface below `reach` above her, facing up (a roof, a
            // branch, the top of a wall), that she can see.
            for (float r : { 250.0f, 500.0f, 750.0f })
                for (int i = 0; i < 12; ++i)
                {
                    const float a = static_cast<float>(i) * static_cast<float>(M_PI) / 6.0f + r * 0.001f;
                    const osg::Vec3f column(feet.x() + std::cos(a) * r, feet.y() + std::sin(a) * r, 0);
                    const MWPhysics::RayCastingResult top = world->getRayCasting()->castRay(
                        osg::Vec3f(column.x(), column.y(), feet.z() + reach), osg::Vec3f(column.x(), column.y(), feet.z() + kMinAbove),
                        { player }, {}, kStatic);
                    ++columns;
                    if (!top.mHit || top.mHitNormal.z() < 0.5f)
                        continue;
                    ++tops;
                    const osg::Vec3f at = top.mHitPos + osg::Vec3f(0, 0, 10.0f);
                    if ((at - feet).length() > reach)
                        continue;
                    // She must see it: nothing in the way but the thing it is on (from below, a roof's edge or a
                    // branch hides its own top).
                    const MWPhysics::RayCastingResult sight
                        = world->getRayCasting()->castRay(head, at + osg::Vec3f(0, 0, 40.0f), { player }, {}, kStatic);
                    if (sight.mHit && (sight.mHitPos - at).length() > 250.0f)
                        continue;
                    kurenai::proto::ProxyTarget target;
                    // Stable while the place is found again: its position on a 48-unit grid.
                    const auto cell = [](float v) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(v / 48.0f))); };
                    target.actorId = (cell(at.x()) * 73856093u) ^ (cell(at.y()) * 19349663u) ^ (cell(at.z()) * 83492791u);
                    target.position = t.toRedNinja({ at.x(), at.y(), at.z() });
                    found.emplace_back((at - feet).length2(), target);
                }
            std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            std::vector<kurenai::proto::ProxyTarget> next;
            // While her wire is out, the place it hangs from stays sent (same id, same place).
            if (mLatest && (mLatest->flags & kurenai::proto::FlagWire))
            {
                const kurenai::proto::Vec3& w = mLatest->wireAnchor;
                for (const kurenai::proto::ProxyTarget& old : mHookTargets)
                {
                    const float dx = old.position.x - w.x, dy = old.position.y - w.y, dz = old.position.z - w.z;
                    if (dx * dx + dy * dy + dz * dz < 200.0f * 200.0f)
                    {
                        next.push_back(old);
                        break;
                    }
                }
            }
            for (const auto& [d, found1] : found)
            {
                if (next.size() >= kMaxTargets)
                    break;
                // A place sent before and found again close by keeps its id and position (no hook point jumps).
                kurenai::proto::ProxyTarget target = found1;
                for (const kurenai::proto::ProxyTarget& old : mHookTargets)
                {
                    const float dx = old.position.x - target.position.x, dy = old.position.y - target.position.y,
                                dz = old.position.z - target.position.z;
                    if (dx * dx + dy * dy + dz * dz < 120.0f * 120.0f)
                    {
                        target = old;
                        break;
                    }
                }
                if (std::none_of(next.begin(), next.end(), [&](const auto& n) { return n.actorId == target.actorId; }))
                    next.push_back(target);
            }
            // A place not found in this scan (an edge seen one moment, missed the next) stays for a second.
            std::map<std::uint32_t, int> misses;
            for (const kurenai::proto::ProxyTarget& n : next)
                misses[n.actorId] = 0;
            for (const kurenai::proto::ProxyTarget& old : mHookTargets)
            {
                if (next.size() >= kMaxTargets || misses.count(old.actorId))
                    continue;
                const int missed = mHookMisses[old.actorId] + 1;
                if (missed <= 4)
                {
                    next.push_back(old);
                    misses[old.actorId] = missed;
                }
            }
            mHookMisses.swap(misses);
            if (mHookScans <= 2 || (next.size() != mHookTargets.size() && (next.empty() || mHookTargets.empty())))
                Log(Debug::Info) << "Kurenai: hook: " << next.size() << " places for her hook (" << tops << " surfaces above her in "
                                 << columns << " columns)";
            mHookTargets.swap(next);
        }
        kurenai::proto::HookTargets msg;
        msg.targets = mHookTargets;
        send(msg);
    }

    void Driver::startSmoke(const osg::Vec3f& at)
    {
        Smoke s;
        s.effectId = "kurenai_smoke_" + std::to_string(++mSmokeBombs);
        s.position = at;
        mSmokes.push_back(s);
        Log(Debug::Info) << "Kurenai: smoke bomb at (" << at.x() << ", " << at.y() << ", " << at.z() << ")";
    }

    void Driver::updateSmoke(float dt)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWBase::MechanicsManager* mechanics = MWBase::Environment::get().getMechanicsManager();
        const MWWorld::Ptr player = world->getPlayerPtr();
        for (Smoke& s : mSmokes)
        {
            s.age += dt;
            if (s.age < kSmokeDelay)
                continue;
            if (!s.drawn && !mSmokeModel.empty())
            {
                // The game's cloud is thick and white from the ground to above her head: one puff in the middle and
                // two rings of them, at two heights.
                const VFS::Path::Normalized model(mSmokeModel);
                world->spawnEffect(model, "", s.position, 3.0f, false, true, s.effectId, true);
                for (int ring = 1; ring <= 2; ++ring)
                    for (int i = 0; i < 6; ++i)
                    {
                        const float a = (static_cast<float>(i) + 0.5f * static_cast<float>(ring)) * static_cast<float>(M_PI) / 3.0f;
                        const float r = mSmokeRadius * 0.35f * static_cast<float>(ring);
                        const osg::Vec3f at = s.position + osg::Vec3f(std::cos(a) * r, std::sin(a) * r, 40.0f * static_cast<float>(ring - 1));
                        world->spawnEffect(model, "", at, 3.0f, false, true, s.effectId, true);
                    }
                s.drawn = true;
            }
            // Everyone in the cloud: blind, choking, and no longer after her.
            std::vector<MWWorld::Ptr> actors;
            mechanics->getActorsInRange(s.position, mSmokeRadius, actors);
            for (const MWWorld::Ptr& a : actors)
            {
                if (a == player || a.getClass().getCreatureStats(a).isDead())
                    continue;
                auto [it, isNew] = mSmoked.try_emplace(a.getBase());
                it->second.actor = a;
                it->second.left = kSmokeAfter;
                if (isNew)
                {
                    ++mSmokedActors;
                    a.getClass().getCreatureStats(a).getSpells().add(smokeSpell());
                    Log(Debug::Info) << "Kurenai: smoke catches " << a.getCellRef().getRefId();
                }
                mechanics->stopCombat(a);
            }
        }
        for (auto it = mSmokes.begin(); it != mSmokes.end();)
        {
            if (it->age >= kSmokeDelay + kSmokeLife)
            {
                world->removeEffect(it->effectId);
                it = mSmokes.erase(it);
            }
            else
                ++it;
        }
        for (auto it = mSmoked.begin(); it != mSmoked.end();)
        {
            it->second.left -= dt;
            if (it->second.left > 0)
            {
                ++it;
                continue;
            }
            const MWWorld::Ptr& a = it->second.actor;
            MWMechanics::Spells& spells = a.getClass().getCreatureStats(a).getSpells();
            if (spells.hasSpell(smokeSpell()))
                spells.remove(smokeSpell());
            it = mSmoked.erase(it);
        }
    }

    void Driver::clearSmoke()
    {
        for (const Smoke& s : mSmokes)
            MWBase::Environment::get().getWorld()->removeEffect(s.effectId);
        mSmokes.clear();
        for (auto& entry : mSmoked)
            entry.second.left = 0;
        updateSmoke(0);
        mItemMotion = 0xffffffffu;
    }

    void Driver::pollModeKey()
    {
        if (mToggleScancode == 0)
            return;
        int count = 0;
        const Uint8* keys = SDL_GetKeyboardState(&count);
        const bool down = mToggleScancode < count && keys[mToggleScancode];
        const bool pressed = down && !mToggleHeld;
        mToggleHeld = down;
        // Not while a menu, the console or a dialogue has the keyboard.
        if (!pressed || MWBase::Environment::get().getWindowManager()->isGuiMode())
            return;
        mMorrowindMode = !mMorrowindMode;
        MWBase::Environment::get().getWindowManager()->messageBox(mMorrowindMode
                ? "Morrowind mode: Morrowind's walk, weapons and HUD"
                : (mPeerAlive ? "Combat mode: Red Ninja" : "Combat mode: Red Ninja (waiting for PCSX2)"));
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
        // Triangle action, R1 lock-on, L2 crouch, L1 block (docs/REVERSE_ENGINEERING.md, controls).
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
            in.buttons |= kurenai::proto::BtnL2; // crouch (wall hug next to a wall)
        if (held(MWInput::A_ToggleSpell))
            in.buttons |= kurenai::proto::BtnL1; // block
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
            else if (const auto* proxies = std::get_if<kurenai::proto::ProxyStatus>(&m.payload))
                handleProxyStatus(*proxies);
            else if (const auto* victim = std::get_if<kurenai::proto::ProxyPose>(&m.payload))
                handleProxyPose(*victim);
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
        updateVictims();
        if (mHud)
        {
            mHud->beat(now);
            updateHud(dt);
        }
        if (mShowStatus)
            updateStatus(dt);
        if (mPoseCallback && mFrames % 120 == 0)
            Log(Debug::Info) << "Kurenai: pose override: " << mPoseCallback->frames() << " frames run, last frame "
                             << mPoseCallback->found() << " bones found, " << mPoseCallback->turned()
                             << " turned, last turn " << osg::RadiansToDegrees(mPoseCallback->lastAngle()) << " deg"
                             << (mPoseCallback->missing().empty() ? "" : ", missing: ") << mPoseCallback->missing();

        const bool alive = mLink->peerAlive(now, mPeerTimeoutNs);
        if (alive != mPeerAlive)
            Log(Debug::Info) << "Kurenai: PCSX2 " << (alive ? "connected" : "stopped responding");
        mPeerAlive = alive;
        pollModeKey();
        const bool drive = mPeerAlive && !mMorrowindMode;
        if (drive && !mControlling)
            takeControl();
        else if (!drive && mControlling)
            releaseControl(mPeerAlive ? "Morrowind mode" : "PCSX2 stopped responding");
        mControlling = drive;
        if (!mControlling)
        {
            // Morrowind mode with PCSX2 connected: a neutral pad, so she stands idle in Red Ninja's world.
            if (mPeerAlive)
            {
                const kurenai::proto::Input idle;
                if (!(idle == mLastInput) || mSinceInput > 0.25f)
                    if (send(idle))
                    {
                        mLastInput = idle;
                        mSinceInput = 0;
                    }
            }
            return;
        }

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
        if (!mCombat || !mAnchored || !mControlling)
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
            // An actor with a proxy takes the game's own hits on that proxy (handleProxyStatus), not these.
            if (const auto id = mProxyIds.find(actor.getBase()); id != mProxyIds.end())
                if (const auto p = mProxies.find(id->second); p != mProxies.end() && p->second.assigned)
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

    void Driver::sendProxyTargets(const kurenai::CoordTransform& t)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world->getPlayerPtr();
        const osg::Vec3f here = player.getRefData().getPosition().asVec3();
        std::vector<MWWorld::Ptr> actors;
        MWBase::Environment::get().getMechanicsManager()->getActorsInRange(here, mProxyRadius, actors);
        std::vector<std::pair<float, MWWorld::Ptr>> near;
        for (const MWWorld::Ptr& a : actors)
            if (a != player && a.getRefData().isEnabled() && !a.getClass().getCreatureStats(a).isDead())
                near.emplace_back((a.getRefData().getPosition().asVec3() - here).length2(), a);
        std::sort(near.begin(), near.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        if (near.size() > kurenai::proto::kMaxProxies)
            near.resize(kurenai::proto::kMaxProxies);

        kurenai::proto::ProxyTargets msg;
        std::map<std::uint32_t, Proxy> keep;
        for (const auto& [d, actor] : near)
        {
            auto [it, isNew] = mProxyIds.emplace(actor.getBase(), mNextProxyId);
            if (isNew)
                ++mNextProxyId;
            const std::uint32_t id = it->second;
            Proxy& p = keep[id];
            if (const auto old = mProxies.find(id); old != mProxies.end())
                p = old->second;
            p.actor = actor;
            const ESM::Position& pos = actor.getRefData().getPosition();
            kurenai::proto::ProxyTarget target;
            target.actorId = id;
            target.position = t.toRedNinja({ pos.pos[0], pos.pos[1], pos.pos[2] });
            // Morrowind faces (sin a, cos a) at yaw a; that is Red Ninja (x, z) = (sin a, -cos a), heading pi - a.
            target.heading = static_cast<float>(M_PI) - pos.rot[2];
            msg.targets.push_back(target);
        }
        mProxies.swap(keep);
        send(msg);
    }

    void Driver::handleProxyStatus(const kurenai::proto::ProxyStatus& status)
    {
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        mLockedProxy = 0;
        for (auto& entry : mProxies)
            entry.second.assigned = false;
        for (const kurenai::proto::ProxyState& s : status.proxies)
        {
            const auto it = mProxies.find(s.actorId);
            if (it == mProxies.end())
                continue;
            Proxy& p = it->second;
            p.assigned = true;
            if (s.flags & kurenai::proto::ProxyLocked)
                mLockedProxy = s.actorId;
            const MWWorld::Ptr& actor = p.actor;
            if (actor.isEmpty() || p.killed || actor.getClass().getCreatureStats(actor).isDead())
                continue;
            if (p.lastHealth == 0)
                p.lastHealth = s.maxHealth;
            const osg::Vec3f hitPos = actor.getRefData().getPosition().asVec3() + osg::Vec3f(0, 0, 100);
            auto& stats = actor.getClass().getCreatureStats(actor);
            float damage = 0;
            const char* how = "hit";
            if (s.flags & kurenai::proto::ProxyDead)
            {
                // The game killed its proxy (stealth kill, dart to the head, blade): the actor dies the same way.
                damage = stats.getHealth().getCurrent() + 1.0f;
                p.killed = true;
                ++mProxyKills;
                how = "killed";
            }
            else if (s.maxHealth > 0 && s.health < p.lastHealth)
            {
                // The same share of its health as the game's enemy lost.
                damage = static_cast<float>(p.lastHealth - std::max(s.health, 0)) / static_cast<float>(s.maxHealth)
                    * stats.getHealth().getModified();
                p.lastHealth = s.health;
            }
            if (damage <= 0)
                continue;
            ++mProxyDamage;
            MWBase::Environment::get().getLuaManager()->onHit(player, actor, MWWorld::Ptr(), MWWorld::Ptr(), 0, 1.0f,
                damage, true, hitPos, true, MWMechanics::DamageSourceType::Melee);
            if (p.killed)
            {
                // Morrowind's combat script scales hit damage (armor, difficulty); a kill in the game is a kill here.
                MWMechanics::DynamicStat<float> health = stats.getHealth();
                health.setCurrent(0);
                stats.setHealth(health);
            }
            Log(Debug::Info) << "Kurenai: proxy " << s.actorId << " (game enemy " << s.slot << ", health " << s.health
                             << "/" << s.maxHealth << ") " << how << ": " << actor.getCellRef().getRefId() << " takes "
                             << damage << " damage";
        }
    }

    void Driver::handleProxyPose(const kurenai::proto::ProxyPose& p)
    {
        auto v = mVictims.find(p.actorId);
        if (v == mVictims.end())
        {
            const auto proxy = mProxies.find(p.actorId);
            if (proxy == mProxies.end() || proxy->second.actor.isEmpty())
                return;
            v = mVictims.emplace(p.actorId, Victim{}).first;
            v->second.actor = proxy->second.actor;
            Log(Debug::Info) << "Kurenai: kill on " << v->second.actor.getCellRef().getRefId() << " (proxy " << p.actorId
                             << ", model " << std::hex << p.model << std::dec << "): its body follows the game's";
        }
        Victim& victim = v->second;
        if (victim.final)
            return;
        ++victim.poses;
        const MWWorld::Ptr& actor = victim.actor;
        const auto map = mSkeletonMaps.find(p.model);
        auto target = std::make_shared<PoseTarget>();
        if (map != mSkeletonMaps.end() && p.model != 0)
        {
            const std::vector<kurenai::AimDirection> dirs = kurenai::aimDirections(p.bones, p.rotation, map->second);
            target->active = true;
            for (std::size_t i = 0; i < dirs.size(); ++i)
            {
                const kurenai::BoneAim& a = map->second.aims[i];
                target->aims.push_back({ a.bone, a.child, osg::Vec3f(dirs[i].dir.x, dirs[i].dir.y, dirs[i].dir.z),
                    a.sideChild, osg::Vec3f(dirs[i].side.x, dirs[i].side.y, dirs[i].side.z) });
            }
            if (const auto root = kurenai::rootOffset(p.bones, p.position, p.rotation, map->second))
            {
                target->rootBone = map->second.rootBone;
                target->rootOffset = osg::Vec3f(root->x, root->y, root->z);
            }
        }
        else if (victim.poses == 1)
            Log(Debug::Warning) << "Kurenai: no skeleton map for the victim's model " << std::hex << p.model << std::dec
                                << "; it keeps Morrowind's animation";
        victim.target = target;
        if (victim.callback)
            victim.callback->setTarget(target);

        // Where the game has the enemy: the actor stands there, facing its way (its height stays Morrowind's).
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const kurenai::proto::Vec3 at = mTransform.position(p.position);
        const osg::Vec3f here = actor.getRefData().getPosition().asVec3();
        if (std::hypot(at.x - here.x(), at.y - here.y()) > 1.0f)
            world->moveObject(actor, osg::Vec3f(at.x, at.y, here.z()));
        const float yaw = mTransform.yaw(p.rotation, { 0, 0, 1 });
        const ESM::Position& pos = actor.getRefData().getPosition();
        if (std::abs(yaw - pos.rot[2]) > 1e-3f)
            world->rotateObject(actor, osg::Vec3f(pos.rot[0], pos.rot[1], yaw));

        if ((p.flags & kurenai::proto::ProxyPoseFinal) && (p.flags & kurenai::proto::ProxyPoseRelease))
        {
            // The wire threw it down and it got up again: it fights on with Morrowind's own animation, from here.
            victim.final = victim.release = true;
            Log(Debug::Info) << "Kurenai: " << actor.getCellRef().getRefId() << " got up after " << victim.poses
                             << " poses; its body is Morrowind's again";
        }
        else if (p.flags & kurenai::proto::ProxyPoseFinal)
        {
            victim.final = true;
            Log(Debug::Info) << "Kurenai: kill on " << actor.getCellRef().getRefId() << " over after " << victim.poses
                             << " poses; the body keeps the game's last pose";
        }
    }

    void Driver::updateVictims()
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        for (auto it = mVictims.begin(); it != mVictims.end();)
        {
            Victim& v = it->second;
            MWRender::Animation* anim = v.actor.isEmpty() || !v.actor.getRefData().getBaseNode()
                ? nullptr
                : world->getAnimation(v.actor);
            osg::Node* skeleton = anim ? anim->getSkeleton() : nullptr;
            osg::ref_ptr<osg::Node> attached;
            v.rootBone.lock(attached);
            if (!skeleton || v.release)
            {
                // Unloaded (cell change) or released (it lives): the body is Morrowind's again.
                if (attached && v.callback)
                    attached->removeUpdateCallback(v.callback);
                it = mVictims.erase(it);
                continue;
            }
            SceneUtil::FindByNameVisitor find("Bip01");
            skeleton->accept(find);
            if (find.mFoundNode && find.mFoundNode != attached.get())
            {
                if (attached && v.callback)
                    attached->removeUpdateCallback(v.callback);
                v.callback = new PoseOverride(v.target, skeleton);
                find.mFoundNode->addUpdateCallback(v.callback);
                v.rootBone = find.mFoundNode;
            }
            // While the game moves the body, Morrowind's animation stands still (as SkipAnim does for mannequins), so
            // its death neither plays nor starts the ragdoll; once the body lies still, the death runs its course
            // under the kept pose.
            if (!v.final)
                MWBase::Environment::get().getMechanicsManager()->skipAnimation(v.actor);
            ++it;
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
        mPoseBones = p.bones;
        if (mPs2Anim == Ps2Anim::Off || !mLatest)
            return;
        // Each model (outfit) has its own bone layout; an unknown one is left to Morrowind's animation.
        auto it = mSkeletonMaps.find(p.model);
        if (it == mSkeletonMaps.end())
            it = mSkeletonMaps.find(0);
        if (p.model != mPoseModel)
        {
            mPoseModel = p.model;
            Log(Debug::Info) << "Kurenai: model " << std::hex << p.model << std::dec << " (" << p.bones.size()
                             << " bones): " << (it != mSkeletonMaps.end() ? "skeleton map found" : "no skeleton map, "
                                                                                                     "Morrowind animates");
        }
        if (it == mSkeletonMaps.end())
        {
            if (mPoseTarget->active)
            {
                mPoseTarget = std::make_shared<PoseTarget>();
                if (mPoseCallback)
                    mPoseCallback->setTarget(mPoseTarget);
            }
            return;
        }
        const kurenai::SkeletonMap& map = it->second;
        const std::vector<kurenai::AimDirection> dirs = kurenai::aimDirections(p.bones, mLatest->rotation, map);
        auto target = std::make_shared<PoseTarget>();
        const auto& loco = map.locomotion;
        target->active = mPs2Anim == Ps2Anim::All
            || std::find(loco.begin(), loco.end(), mLatest->animId) != loco.end();
        for (std::size_t i = 0; i < dirs.size(); ++i)
        {
            const kurenai::BoneAim& a = map.aims[i];
            target->aims.push_back({ a.bone, a.child, osg::Vec3f(dirs[i].dir.x, dirs[i].dir.y, dirs[i].dir.z),
                a.sideChild, osg::Vec3f(dirs[i].side.x, dirs[i].side.y, dirs[i].side.z) });
        }
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
        else if (!mControlling)
            s << "Morrowind mode (PCSX2 connected, idle)";
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
            if (!mProxies.empty())
            {
                std::size_t assigned = 0;
                for (const auto& entry : mProxies)
                    assigned += entry.second.assigned;
                s << "  proxies " << assigned << "/" << mProxies.size();
                if (const auto it = mProxies.find(mLockedProxy); it != mProxies.end() && !it->second.actor.isEmpty())
                    s << "  locked " << it->second.actor.getCellRef().getRefId().toDebugString();
            }
            if (mPs2Anim != Ps2Anim::Off)
                s << "  ps2 pose " << (mPoseTarget->active ? "on" : "off");
        }
        mStatus->setCaption(s.str());
    }

    void Driver::hideHud()
    {
        for (MyGUI::ImageBox* box : mHudBoxes)
            box->setVisible(false);
        if (mMorrowindHudHidden)
        {
            MWBase::Environment::get().getWindowManager()->setHudVisibility(true);
            mMorrowindHudHidden = false;
        }
    }

    void Driver::updateHud(float dt)
    {
        mSinceHudFrame += dt;
        if (!mControlling)
        {
            hideHud();
            return;
        }
        if (mHud->read(mHudFrame, mHudSeq))
        {
            mSinceHudFrame = 0;
            ++mHudFrames;
            const kurenai::HudFrame& f = mHudFrame;
            // One texture per region, holding only the pixels the region owns (regions can overlap on the game's
            // screen, but each goes to its own corner here). GL textures are a power of two: the region sits in
            // the top-left corner.
            if (mHudTextures.size() < f.regions.size())
                mHudTextures.resize(f.regions.size());
            MyGUI::RenderManager& rm = MyGUI::RenderManager::getInstance();
            for (std::size_t i = 0; i < f.regions.size(); ++i)
            {
                const kurenai::HudRegion& r = f.regions[i];
                HudTexture& t = mHudTextures[i];
                int tw = 1, th = 1;
                while (tw < r.w)
                    tw *= 2;
                while (th < r.h)
                    th *= 2;
                if (!t.texture || t.size[0] != tw || t.size[1] != th)
                {
                    if (t.texture)
                        rm.destroyTexture(t.texture);
                    t.name = "kurenai_game_hud_" + std::to_string(i);
                    t.texture = rm.createTexture(t.name);
                    t.texture->createManual(tw, th, MyGUI::TextureUsage::Write, MyGUI::PixelFormat::R8G8B8A8);
                    t.size[0] = tw;
                    t.size[1] = th;
                }
                if (auto* data = static_cast<std::uint32_t*>(t.texture->lock(MyGUI::TextureUsage::Write)))
                {
                    std::fill_n(data, static_cast<std::size_t>(tw) * th, 0u);
                    const std::uint8_t own = static_cast<std::uint8_t>(i + 1);
                    for (int y = 0; y < r.h && r.y + y < static_cast<int>(f.height); ++y)
                        for (int x = 0; x < r.w && r.x + x < static_cast<int>(f.width); ++x)
                        {
                            const std::size_t at = static_cast<std::size_t>(r.y + y) * f.width + r.x + x;
                            if (f.owner[at] == own)
                                data[static_cast<std::size_t>(y) * tw + x] = f.pixels[at];
                        }
                }
                t.texture->unlock();
            }

            // One image per HUD region, kept at the same distance from its nearest screen corner as in the game.
            const MyGUI::IntSize view = MyGUI::RenderManager::getInstance().getViewSize();
            const float scale = mHudScale * view.height / static_cast<float>(f.height);
            while (mHudBoxes.size() < f.regions.size())
            {
                MyGUI::ImageBox* box = MyGUI::Gui::getInstance().createWidget<MyGUI::ImageBox>(
                    "ImageBox", MyGUI::IntCoord(0, 0, 1, 1), MyGUI::Align::Default, "HUD");
                box->setNeedMouseFocus(false);
                mHudBoxes.push_back(box);
            }
            for (std::size_t i = 0; i < mHudBoxes.size(); ++i)
            {
                MyGUI::ImageBox* box = mHudBoxes[i];
                if (i >= f.regions.size())
                {
                    box->setVisible(false);
                    continue;
                }
                const kurenai::HudRegion& r = f.regions[i];
                // Kept at the same distance from its nearest screen edge; a region across the middle stays centred.
                auto place = [scale](int at, int size, int screen, int view) {
                    if (at < screen / 2 && at + size > screen / 2)
                        return static_cast<int>(std::lround(view / 2.0f + (at - screen / 2.0f) * scale));
                    if (at + size / 2 > screen / 2)
                        return view - static_cast<int>(std::lround((screen - at) * scale));
                    return static_cast<int>(std::lround(at * scale));
                };
                const int w = static_cast<int>(std::lround(r.w * scale));
                const int h = static_cast<int>(std::lround(r.h * scale));
                box->setCoord(place(r.x, r.w, static_cast<int>(f.width), view.width),
                    place(r.y, r.h, static_cast<int>(f.height), view.height), w, h);
                box->setImageTexture(mHudTextures[i].name);
                box->setImageCoord(MyGUI::IntCoord(0, 0, r.w, r.h));
                box->setImageTile(MyGUI::IntSize(r.w, r.h));
                box->setVisible(true);
            }
            if (mHideMorrowindHud && !mMorrowindHudHidden)
            {
                MWBase::Environment::get().getWindowManager()->setHudVisibility(false);
                mMorrowindHudHidden = true;
            }
            if (mHudFrames == 1)
                Log(Debug::Info) << "Kurenai: game HUD: first frame " << f.width << "x" << f.height << ", "
                                 << f.regions.size() << " regions, drawn at " << scale << "x";
        }
        else if (mSinceHudFrame > 1.0f)
            hideHud(); // PCSX2 stopped sending the HUD (no HudLayout, or a menu without one)
    }

    void Driver::updateWire(const kurenai::CoordTransform& t)
    {
        bool show = false;
        osg::Vec3f hand, anchor;
        if ((mLatest->flags & kurenai::proto::FlagWire) && mPoseModel != 0xffffffffu)
        {
            const auto it = mSkeletonMaps.find(mPoseModel);
            if (it != mSkeletonMaps.end() && it->second.wireHand && *it->second.wireHand < mPoseBones.size())
            {
                const kurenai::proto::Vec3 h = t.position(mPoseBones[*it->second.wireHand]);
                const kurenai::proto::Vec3 a = t.position(mLatest->wireAnchor);
                hand.set(h.x, h.y, h.z);
                anchor.set(a.x, a.y, a.z);
                show = true;
            }
        }
        if (!mWireGeometry)
        {
            if (!show)
                return;
            mWireGeometry = new osg::Geometry;
            mWireVertices = new osg::Vec3Array(2);
            mWireGeometry->setVertexArray(mWireVertices);
            osg::ref_ptr<osg::Vec4Array> colour = new osg::Vec4Array(1);
            (*colour)[0] = osg::Vec4f(0.15f, 0.13f, 0.12f, 1.0f); // dark steel wire
            mWireGeometry->setColorArray(colour, osg::Array::BIND_OVERALL);
            mWireGeometry->addPrimitiveSet(new osg::DrawArrays(GL_LINES, 0, 2));
            mWireGeometry->setDataVariance(osg::Object::DYNAMIC);
            mWireGeometry->setUseDisplayList(false);
            mWireGeometry->setCullingActive(false);
            osg::StateSet* ss = mWireGeometry->getOrCreateStateSet();
            ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
            ss->setAttributeAndModes(new osg::LineWidth(2.5f));
            mWireGeometry->setNodeMask(MWRender::Mask_Effect);
            MWBase::Environment::get().getWorld()->getRenderingManager()->getLightRoot()->addChild(mWireGeometry);
            Log(Debug::Info) << "Kurenai: drawing her wire";
        }
        if (show)
        {
            (*mWireVertices)[0] = hand;
            (*mWireVertices)[1] = anchor;
            mWireVertices->dirty();
            mWireGeometry->dirtyBound();
        }
        mWireGeometry->setNodeMask(show ? MWRender::Mask_Effect : 0u);
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
        osg::Node* rootBone = nullptr;
        if (skeleton)
        {
            SceneUtil::FindByNameVisitor find("Bip01");
            skeleton->accept(find);
            rootBone = find.mFoundNode;
        }
        osg::ref_ptr<osg::Node> current;
        if (mPoseSkeleton.lock(current) && current.get() == rootBone)
            return;
        if (current && mPoseCallback)
            current->removeUpdateCallback(mPoseCallback);
        mPoseSkeleton = rootBone;
        if (!rootBone)
            return;
        mPoseCallback = new PoseOverride(mPoseTarget, skeleton);
        rootBone->addUpdateCallback(mPoseCallback);
        Log(Debug::Info) << "Kurenai: PS2 animation (" << (mPs2Anim == Ps2Anim::All ? "all" : "locomotion")
                         << ") attached to the player's skeleton; " << mSkeletonMaps.size() << " skeleton maps";
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
        if (!mSmokes.empty() || !mSmoked.empty())
            updateSmoke(dt);
        if (!mLink || !mControlling || !mLatest)
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
        kurenai::CoordTransform t;
        {
            std::lock_guard<std::mutex> lock(mTransformMutex);
            t = mTransform;
        }
        const float blend = (mPrev && mInterpolate) ? std::fmin(1.0f, mSinceLatest * kGameHz) : 1.0f;
        const kurenai::proto::PlayerState& a = mPrev ? *mPrev : *mLatest;
        const kurenai::proto::Vec3 rn = lerp(a.position, mLatest->position, blend);
        kurenai::proto::Vec3 mw = t.position(rn);
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
        const float yaw = lerpAngle(t.yaw(a.rotation, { 0, 0, 1 }), t.yaw(mLatest->rotation, { 0, 0, 1 }), blend);

        world->moveObject(player, osg::Vec3f(mw.x, mw.y, mw.z), true, true);
        player = world->getPlayerPtr();
        world->rotateObject(player, osg::Vec3f(0, 0, yaw), MWBase::RotationFlag_none);
        if (mProxyEnabled)
            sendProxyTargets(t);
        if (mHooks)
            sendHookTargets(t, dt);
        applyAnimation(mLatest->animId);
        updateSneak(mLatest->animId);
        updateKiton(mLatest->animId, mLatest->kiton);
        if (mLatest->animId != mItemMotion)
        {
            mItemMotion = mLatest->animId;
            if (smokeMotions().count(mItemMotion))
                startSmoke(player.getRefData().getPosition().asVec3());
        }
        updatePoseOverride();
        updateWire(t);
        if (mGameCameraMode)
            applyGameCamera();
        if (mLog)
            std::fprintf(mLog, "%llu\t%llu\t\t\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.4f\t%u\n",
                static_cast<unsigned long long>(mFrames), static_cast<unsigned long long>(kurenai::monotonicNs()),
                rn.x, rn.y, rn.z, mw.x, mw.y, mw.z, yaw, mLatest->animId);
    }
}
