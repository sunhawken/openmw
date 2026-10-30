#include "driver.hpp"
#include "nativebody.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
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
#include "../mwbase/statemanager.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwgui/mode.hpp"
#include "../mwinput/actions.hpp"
#include "../mwmechanics/aisequence.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwworld/worldmodel.hpp"
#include "../mwmechanics/damagesourcetype.hpp"
#include "../mwmechanics/drawstate.hpp"
#include "../mwmechanics/movement.hpp"
#include "../mwmechanics/npcstats.hpp"
#include "../mwphysics/joltlayers.hpp"
#include "../mwphysics/raycasting.hpp"
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/visitor.hpp>

#include "../mwrender/animation.hpp"
#include "../mwrender/camera.hpp"
#include "../mwrender/renderingmanager.hpp"
#include "../mwrender/vismask.hpp"

#include <components/sceneutil/lightmanager.hpp>

#include "../mwmechanics/spells.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/ptr.hpp"
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/vfs/manager.hpp>
#include <osg/ComputeBoundsVisitor>
#include <osg/Geometry>
#include <osg/LineWidth>

#include <components/esm/defs.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadspel.hpp>

namespace MWKurenai
{
    namespace
    {
        Driver* sInstance = nullptr; // the one driver, for the engine's death handling (deferDeath)
    }

    namespace
    {
        constexpr float kGameHz = 30.0f;
        // Her thrown kunai's speed, game units per game frame.
        constexpr float kKunaiSpeed = 40.0f;
        constexpr float kDiveSpeed = 150.0f; // Morrowind units per second, swimming up or down
        constexpr float kSurfaceDepth = 8.0f; // shallower than this (Morrowind units) she is at the surface
        // While she swims, the game wants a bottom within its ground probe's reach (310 below her) or it has her fall
        // out of the water (runs/swim4: 1191 -> 1000 -> 1150 over and over in Morrowind's deep sea). Deeper water gets
        // a bottom this far below the surface (game units): the moat's (runs/swimM). The bottom under her water is
        // reported with the game's water-bottom surface type (proto::kMaterialWaterFloor), as the moat's is.
        constexpr float kSwimFloor = 225.0f;
        // How long a collision query waits for the world lock (see queryLock), and once the world has stayed locked.
        constexpr std::chrono::milliseconds kQueryLockWait{ 20 };
        constexpr std::chrono::milliseconds kStalledLockWait{ 1 };

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

        // Her motions in the water ("swim<TAB>id id ..."): treading, swimming, the breathing tube; Morrowind's drowning
        // is held off while one plays.
        std::set<std::uint32_t>& swimMotions()
        {
            static std::set<std::uint32_t> motions;
            return motions;
        }

        // Her breathing-tube motions (L2 held in the water, "swim_hidden<TAB>id id ..."): she can dive and rise.
        std::set<std::uint32_t>& hiddenSwimMotions()
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
        // Her smoke bomb's throw in Morrowind: launch speed and gravity (Morrowind units), and the arc's step and length.
        // Needs measuring against the game's own throw; about 14 m at the best elevation.
        constexpr float kSmokeSpeed = 850.0f;
        constexpr float kSmokeGravity = 700.0f;
        constexpr float kSmokeStep = 1.0f / 30.0f;
        constexpr float kSmokeMaxTime = 4.0f;
        constexpr float kSmokeTapTime = 0.15f; // long enough for the game to see the button (it reads the pad at 30 Hz)

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
            swimMotions().clear();
            hiddenSwimMotions().clear();
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
                if (line.rfind("sneak", 0) == 0 || line.rfind("kiton", 0) == 0 || line.rfind("smoke", 0) == 0
                    || line.rfind("swim", 0) == 0)
                {
                    std::string word;
                    fields >> word;
                    std::set<std::uint32_t>& set = word == "sneak" ? sneakMotions()
                        : word == "kiton"                          ? kitonMotions()
                        : word == "swim"                           ? swimMotions()
                        : word == "swim_hidden"                    ? hiddenSwimMotions()
                                                                   : smokeMotions();
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
            mPredict = k.mPredict;
            mAnswerCollision = k.mAnswerCollision;
            mCombat = k.mCombat;
            mGameCameraMode = k.mCameraMode.get() == "game";
            mGameLens = k.mGameLens;
            mHealthMode = k.mHealth;
            mMaxViewDistance = k.mMaxViewDistance;
            mShowStatus = k.mShowStatus;
            mHideMorrowindHud = k.mHideMorrowindHud;
            mHudScale = k.mGameHudScale;
            mProxyEnabled = k.mProxies;
            mProxyRadius = k.mProxyRadius;
            mLockOn = k.mLockOn;
            mLockRange = k.mLockRange;
            mRadarRegion = k.mRadarRegion;
            mRadarRange = k.mRadarRange;
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
            if (!k.mRecoverKey.get().empty())
            {
                mRecoverScancode = SDL_GetScancodeFromName(k.mRecoverKey.get().c_str());
                if (mRecoverScancode == SDL_SCANCODE_UNKNOWN)
                    Log(Debug::Error) << "Kurenai: unknown recover key '" << k.mRecoverKey.get() << "'";
            }
            if (!k.mControlsKey.get().empty())
            {
                mControlsScancode = SDL_GetScancodeFromName(k.mControlsKey.get().c_str());
                if (mControlsScancode == SDL_SCANCODE_UNKNOWN)
                    Log(Debug::Error) << "Kurenai: unknown controls key '" << k.mControlsKey.get() << "'";
            }
            if (!k.mSelectKey.get().empty())
            {
                mSelectScancode = SDL_GetScancodeFromName(k.mSelectKey.get().c_str());
                if (mSelectScancode == SDL_SCANCODE_UNKNOWN)
                    Log(Debug::Error) << "Kurenai: unknown select key '" << k.mSelectKey.get() << "'";
            }
            {
                std::istringstream words(k.mJoystickMap.get());
                for (std::string w; words >> w;)
                    mJoyMap.push_back(w);
            }
            if (!k.mWeaponKey.get().empty())
            {
                mWeaponScancode = SDL_GetScancodeFromName(k.mWeaponKey.get().c_str());
                if (mWeaponScancode == SDL_SCANCODE_UNKNOWN)
                    Log(Debug::Error) << "Kurenai: unknown weapon key '" << k.mWeaponKey.get() << "'";
            }
            if (k.mNativeBody && !k.mNativeBodyModels.get().empty())
            {
                mNative = std::make_unique<NativeBody>(k.mNativeBodyModels.get());
                mNative->setAttachment(mAttachment);
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
        mPs2 = EmbeddedPs2::start(name);
    }

    Driver::~Driver()
    {
        if (sInstance == this)
            sInstance = nullptr;
        if (mHudFrames)
            Log(Debug::Info) << "Kurenai: game HUD: " << mHudFrames << " frames shown";
        if (mLatency.count)
            Log(Debug::Info) << "Kurenai: PlayerState latency PCSX2 -> OpenMW: transport average "
                             << mLatency.transportSumUs / mLatency.count << " us (max " << mLatency.transportMaxUs
                             << "), until applied in a frame average " << mLatency.sumUs / mLatency.count
                             << " us (max " << mLatency.maxUs << ") over " << mLatency.count << " states";
        mPs2.reset(); // the game first: it still talks to the bridge
        mStop = true;
        if (mThread.joinable())
            mThread.join();
        if (mProxyDamage)
            Log(Debug::Info) << "Kurenai: proxies: " << mProxyDamage << " hits passed on, " << mProxyKills << " kills";
        Log(Debug::Info) << "Kurenai: lock-on: " << mLockPicks << " targets picked, " << mLockMisses
                         << " presses with nothing to lock on";
        if (mSmokeBombs)
            Log(Debug::Info) << "Kurenai: smoke bombs: " << mSmokeBombs << ", " << mSmokedActors << " actors caught";
        if (mStealthKills)
            Log(Debug::Info) << "Kurenai: combat: " << mStealthKills << " stealth kills without a stage guard";
        if (mCombatEvents)
            Log(Debug::Info) << "Kurenai: combat: " << mCombatEvents << " blade segments, " << mCombatHits
                             << " hits on Morrowind actors";
        if (mQueries)
            Log(Debug::Info) << "Kurenai: answered " << mQueries << " collision queries (" << mQueryHits << " hits, "
                             << mQueriesDeclined << " declined, " << mQueriesStalled << " of them while the world loaded), average " << (mQueryNs / 1000.0 / mQueries)
                             << " us each, of which waiting for the world lock " << (mQueryLockNs / 1000.0 / mQueries)
                             << " us (max " << mQueryLockMaxNs / 1000.0 << " us)";
        if (mWaterHits)
            Log(Debug::Info) << "Kurenai: water: " << mWaterHits << " water tests met Morrowind's water, "
                             << mSwimFloorHits << " ground probes met the swimming bottom";
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
            // The heartbeat goes from here as well as from the frame: a long frame (a cell loading) must not look like
            // a closed OpenMW to PCSX2, which then drops the link and her floor with it. Only once a game runs (PCSX2
            // must not connect before the player is there) and for at most 30 s after the frame's last one.
            const std::uint64_t frameBeat = mFrameBeatNs.load(std::memory_order_relaxed);
            if (frameBeat != 0 && now - frameBeat < 30'000'000'000ull)
                mLink->tick(now);
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
        kurenai::proto::Vec3 a = t.position(q.from), b = t.position(q.to);
        if (q.mask & kurenai::proto::kQueryProjectile)
        {
            // Her kunai leaving her hand: drawn by updateKunai, nothing waits for an answer.
            std::lock_guard<std::mutex> lock(mKunaiMutex);
            if (mKunaiNew.size() < 16)
                mKunaiNew.push_back({ osg::Vec3f(a.x, a.y, a.z), osg::Vec3f(b.x, b.y, b.z) });
            return;
        }
        if (q.mask & kurenai::proto::kQueryWater)
        {
            answerWater(q, t, a, b);
            return;
        }
        // Swimming: the bottom she may not reach in deep water (see kSwimFloor), on Red Ninja's side of the dive shift.
        const bool swimming = mSwimming.load();
        const float floorZ = mWaterLevel.load() - kSwimFloor * t.scale;
        const kurenai::proto::Vec3 fromMw = a, toMw = b;
        // Diving with the breathing tube: the game keeps her at the surface; Morrowind has her this far below it, so
        // the walls and floor she meets are the ones down there.
        const float depth = mSwimDepth.load();
        a.z -= depth;
        b.z -= depth;
        MWPhysics::RayCastingResult hit;
        {
            const std::uint64_t lockStart = kurenai::monotonicNs();
            const std::unique_lock<std::timed_mutex> lock = queryLock();
            const std::uint64_t waited = kurenai::monotonicNs() - lockStart;
            mQueryLockNs += waited;
            if (waited > mQueryLockMaxNs)
                mQueryLockMaxNs = waited;
            if (!lock.owns_lock())
            {
                res.material = kurenai::proto::kMaterialDeclined;
                ++mQueriesDeclined;
                ++mQueriesStalled;
                send(res);
                return;
            }
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
            res.point = t.toRedNinja({ p.x(), p.y(), p.z() + depth });
            osg::Vec3f n = hit.mHitNormal;
            if (n.normalize() == 0)
                n = osg::Vec3f(0, 0, 1);
            res.normal = kurenai::CoordTransform::axesInverse({ n.x(), n.y(), n.z() });
            const float len = (osg::Vec3f(b.x, b.y, b.z) - osg::Vec3f(a.x, a.y, a.z)).length();
            res.fraction = len > 0 ? std::fmin(1.0f, (p - osg::Vec3f(a.x, a.y, a.z)).length() / len) : 0.0f;
            ++mQueryHits;
        }
        // Only her ground probe (310 straight down) meets it: the moat's bottom is out of reach of the others.
        const bool groundProbe = std::fabs(q.from.y - q.to.y) > 300.0f && std::fabs(q.from.x - q.to.x) < 1.0f
            && std::fabs(q.from.z - q.to.z) < 1.0f;
        if (swimming && groundProbe && fromMw.z > floorZ && toMw.z < floorZ
            && (!hit.mHit || hit.mHitPos.z() + depth < floorZ))
        {
            const float f = (fromMw.z - floorZ) / (fromMw.z - toMw.z);
            res.hit = true;
            res.fraction = f;
            res.point = t.toRedNinja({ fromMw.x + (toMw.x - fromMw.x) * f, fromMw.y + (toMw.y - fromMw.y) * f, floorZ });
            res.normal = kurenai::CoordTransform::axesInverse({ 0, 0, 1 });
            ++mSwimFloorHits;
        }
        if (swimming && res.hit && res.normal.y > 0.7f && t.position(res.point).z < mWaterLevel.load())
            res.material = kurenai::proto::kMaterialWaterFloor;
        send(res);
        ++mQueries;
        mQueryNs += kurenai::monotonicNs() - start;
    }

    // A world update holds the world lock for a millisecond or two, a cell loading for up to seconds, and the game waits
    // for every answer (up to its CollisionTimeoutUs, 100 ms). So a query that can't get the lock within
    // kQueryLockWait is declined (the game replays Morrowind's last answer for that caller), and while the world stays
    // locked the next ones give up after kStalledLockWait, so a load costs her one short wait, not one per query.
    std::unique_lock<std::timed_mutex> Driver::queryLock()
    {
        std::unique_lock<std::timed_mutex> lock(mWorldMutex, std::defer_lock);
        mWorldStalled = !lock.try_lock_for(mWorldStalled ? kStalledLockWait : kQueryLockWait);
        return lock;
    }

    void Driver::answerWater(const kurenai::proto::CollisionQuery& q, const kurenai::CoordTransform& t,
        const kurenai::proto::Vec3& a, const kurenai::proto::Vec3& b)
    {
        // The game's water test: a segment down through where its water surface would be. Morrowind's water is a level
        // plane in each cell that has water (every exterior).
        kurenai::proto::CollisionResult res;
        res.queryId = q.queryId;
        bool wet = false;
        float level = 0;
        {
            const std::unique_lock<std::timed_mutex> lock = queryLock();
            if (!lock.owns_lock())
            {
                res.material = kurenai::proto::kMaterialDeclined;
                ++mQueriesDeclined;
                ++mQueriesStalled;
                send(res);
                return;
            }
            const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
            const MWWorld::CellStore* cell = player.isInCell() ? player.getCell() : nullptr;
            if (cell && cell->getCell()->hasWater())
            {
                wet = true;
                level = cell->getWaterLevel();
            }
        }
        if (wet && a.z != b.z && (a.z - level) * (b.z - level) <= 0)
        {
            const float f = (a.z - level) / (a.z - b.z);
            res.hit = true;
            res.fraction = f;
            res.point = t.toRedNinja({ a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, level });
            res.normal = kurenai::CoordTransform::axesInverse({ 0, 0, 1 });
            res.material = kurenai::proto::kMaterialWaterSurface;
            ++mQueryHits;
            ++mWaterHits;
        }
        send(res);
        ++mQueries;
    }

    void Driver::updateSwim(float dt, kurenai::proto::Vec3& mw)
    {
        const std::uint32_t motion = mLatest->animId;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world->getPlayerPtr();
        const MWWorld::CellStore* cell = player.isInCell() ? player.getCell() : nullptr;
        if (!swimMotions().count(motion) || !cell || !cell->getCell()->hasWater())
        {
            mSwimDepth = 0;
            mSwimming = false;
            return;
        }
        mWaterLevel = cell->getWaterLevel();
        mSwimming = true;
        // Morrowind's swimming: forward goes where the camera looks (look down to dive, up to rise) and Jump swims up.
        // The game's own swim motions keep playing; it only swims at the surface, so the depth is ours. Her breathing
        // tube (L2, sendInput) only comes out at the surface and stays there.
        const MWBase::InputManager* input = MWBase::Environment::get().getInputManager();
        float depth = mSwimDepth.load();
        if (hiddenSwimMotions().count(motion))
            depth = 0;
        else
        {
            const float forward
                = input->getActionValue(MWInput::A_MoveForward) - input->getActionValue(MWInput::A_MoveBackward);
            depth += forward * std::sin(mSwimPitch) * kDiveSpeed * dt;
            if (input->getActionValue(MWInput::A_Jump) > 0.5f)
                depth -= kDiveSpeed * dt;
        }
        // At the surface (and in the tube) she breathes; below it Morrowind's breath and drowning run.
        if (depth < kSurfaceDepth)
        {
            static const float holdBreath = MWBase::Environment::get()
                                                .getESMStore()
                                                ->get<ESM::GameSetting>()
                                                .find("fHoldBreathTime")
                                                ->mValue.getFloat();
            player.getClass().getNpcStats(player).setTimeToStartDrowning(holdBreath);
        }
        // Never into the bottom: stop a body's height above whatever is below the surface.
        const MWPhysics::RayCastingResult floor = world->getRayCasting()->castRay(osg::Vec3f(mw.x, mw.y, mw.z),
            osg::Vec3f(mw.x, mw.y, mw.z - 20000.0f), {}, {},
            MWPhysics::Layers::WORLD | MWPhysics::Layers::HEIGHTMAP | MWPhysics::Layers::DOOR);
        const float room = floor.mHit ? std::fmax(0.0f, mw.z - floor.mHitPos.z() - 40.0f) : 20000.0f;
        depth = std::fmax(0.0f, std::fmin(depth, room));
        mSwimDepth = depth;
        mw.z -= depth;
    }

    void Driver::takeControl()
    {
        Log(Debug::Info) << "Kurenai: combat mode: Red Ninja's code now drives the player";
        // Re-anchor on her next state. Until then the previous anchor keeps answering the game's collision queries
        // (she hasn't moved in Red Ninja's world meanwhile), so no query falls back to the stage's own geometry.
        mReanchor = true;
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
        if (mKunaiNode)
            mKunaiNode->setNodeMask(0u);
        mKunai = Kunai::Hidden;
        releaseCamera();
        osg::ref_ptr<osg::Node> skeleton;
        if (mPoseSkeleton.lock(skeleton) && mPoseCallback)
            skeleton->removeUpdateCallback(mPoseCallback);
        mPoseSkeleton = nullptr;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (mNative)
        {
            mNative->update(world->getPlayerPtr(), false, nullptr, nullptr, mTransform.scale);
            mNativeShown = false;
        }
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
        if (!mLockActor.isEmpty())
            send(kurenai::proto::LockTarget{});
        mLockActor = MWWorld::Ptr();
        mLockLast = MWWorld::Ptr();
        mLockHeld = false;
        if (mLockMarker)
            mLockMarker->setVisible(false);
    }

    void Driver::updateNinjutsu(std::uint32_t motion, float dt)
    {
        // L1+Square in the game: her activation motion 1450 starts bullet time. Everything but her slows down; she is
        // driven by the PS2 so only Morrowind's simulation scale is lowered.
        constexpr float kDuration = 12.f;
        constexpr float kScale = 0.3f;
        auto* tm = MWBase::Environment::get().getWorld()->getTimeManager();
        if (motion == 1450 && mNinjutsuPrev != 1450)
        {
            mNinjutsuLeft = mNinjutsuLeft > 0.f ? 0.f : kDuration; // pressing it again ends it, like the game
            Log(Debug::Info) << "Kurenai: ninjutsu " << (mNinjutsuLeft > 0.f ? "on" : "off");
            if (mNinjutsuLeft <= 0.f)
                tm->setSimulationTimeScale(1.f);
        }
        mNinjutsuPrev = motion;
        if (mNinjutsuLeft <= 0.f)
            return;
        tm->setSimulationTimeScale(kScale);
        mNinjutsuLeft -= dt; // dt is real time
        if (mNinjutsuLeft <= 0.f)
        {
            mNinjutsuLeft = 0.f;
            tm->setSimulationTimeScale(1.f);
            Log(Debug::Info) << "Kurenai: ninjutsu off";
        }
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
        {
            spells.add(kitonSpell());
            // Enemies lose her (the game's guards give up the chase): Chameleon only stops new ones noticing her,
            // so those already fighting her stop, and must find her again past the Chameleon to resume.
            MWBase::MechanicsManager* mechanics = MWBase::Environment::get().getMechanicsManager();
            const std::vector<MWWorld::Ptr> fighting = mechanics->getActorsFighting(player);
            for (const MWWorld::Ptr& actor : fighting)
                actor.getClass().getCreatureStats(actor).getAiSequence().stopCombat({ player });
            if (!fighting.empty())
                Log(Debug::Info) << "Kurenai: kiton: " << fighting.size() << " enemies lost her";
        }
        else if (spells.hasSpell(kitonSpell()))
            spells.remove(kitonSpell());
    }

    void Driver::sendMorrowindHealth()
    {
        // [Kurenai] health = morrowind: Morrowind's health counts. Sent every frame while PCSX2 is connected, in
        // Morrowind mode too, since the game keeps running there: it shows the share on its bar and keeps her alive.
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        const MWMechanics::DynamicStat<float>& hp = player.getClass().getCreatureStats(player).getHealth();
        kurenai::proto::PlayerHealth m;
        m.fraction = hp.getModified() > 0 ? std::clamp(hp.getCurrent() / hp.getModified(), 0.0f, 1.0f) : 0.0f;
        if (std::fabs(hp.getCurrent() - mLastGameHealth) >= 0.5f)
        {
            Log(Debug::Info) << "Kurenai: Morrowind health " << hp.getCurrent() << " / " << hp.getModified()
                             << " -> the game's bar";
            mLastGameHealth = hp.getCurrent();
        }
        send(m);
    }

    void Driver::updateHealth(float health, float healthMax)
    {
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
        // healthMax 0: the game's object or the layout's health fields are missing; leave Morrowind's alone.
        if (mHealthMode != "game" || healthMax <= 0)
            return;
        if (stats.isDead()) // the game's continue refills her bar; Morrowind's death stands (load a save)
            return;
        MWMechanics::DynamicStat<float> hp = stats.getHealth();
        const float target = hp.getModified() * std::clamp(health / healthMax, 0.0f, 1.0f);
        if (std::fabs(hp.getCurrent() - target) < 0.01f)
            return;
        if (health != mLastGameHealth)
        {
            Log(Debug::Info) << "Kurenai: health " << health << " / " << healthMax << " -> " << target << " / "
                             << hp.getModified();
            mLastGameHealth = health;
        }
        hp.setCurrent(target, true);
        stats.setHealth(hp);
    }

    // A potion or elixir the game used for her (PCSX2 reports it in the top bits of the state flags): what it healed,
    // in percent of her maximum, goes onto Morrowind's health, which is the one that counts in that mode.
    void Driver::applyHeals(std::uint32_t flags)
    {
        const int count = static_cast<int>((flags >> kurenai::proto::kHealCountShift) & 0xffu);
        const float percent = static_cast<float>(flags >> kurenai::proto::kHealPercentShift);
        if (mHealSeen < 0)
        {
            mHealSeen = count;
            return;
        }
        const int times = (count - mHealSeen) & 0xff;
        mHealSeen = count;
        if (times == 0 || percent <= 0 || mHealthMode != "morrowind")
            return;
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
        if (stats.isDead())
            return;
        MWMechanics::DynamicStat<float> hp = stats.getHealth();
        const float before = hp.getCurrent();
        hp.setCurrent(std::min(hp.getModified(), before + hp.getModified() * percent / 100.0f * static_cast<float>(times)), true);
        stats.setHealth(hp);
        Log(Debug::Info) << "Kurenai: potion healed " << percent << " % (x" << times << "): Morrowind health " << before << " -> "
                         << hp.getCurrent() << " / " << hp.getModified();
    }

    void Driver::sendHookTargets(const kurenai::CoordTransform& t, float dt)
    {
        // The game's lock-on takes hook points up to 1500 of its units away (0x0021df00).
        constexpr float kGameReach = 1400.0f;
        constexpr float kMinAbove = 220.0f; // Morrowind units above her feet: a hook is for going up or across
        constexpr std::size_t kMaxTargets = 8;
        // Holding △ with the Hook (1264, then 1260) aims it by hand: the game picks the hook point nearest its view,
        // turns its camera onto it and ignores the right stick. Once the right stick moves during the aim, the engine
        // sends one place only, where a ray from her head meets any Morrowind surface (a wall, a ceiling, a branch,
        // the ground; any height), and the stick steers that ray from the place the game had picked. The game's cyan
        // marker and camera follow the place; on release the Hook flies to it. Untouched, the aim is the game's own.
        constexpr std::uint32_t kStick = kurenai::proto::FlagRightUp | kurenai::proto::FlagRightDown
            | kurenai::proto::FlagRightLeft | kurenai::proto::FlagRightRight;
        const bool aiming = mLatest && (mLatest->animId == 1264 || mLatest->animId == 1260);
        if (aiming && !mHookAiming && (mLatest->flags & kStick))
        {
            mHookAiming = true;
            mAimHit = false;
            const osg::Vec3f head = mGameFeet + osg::Vec3f(0, 0, 120.0f);
            osg::Vec3f view(std::sin(mAimYaw), std::cos(mAimYaw), 0);
            if (mLatest->flags & kurenai::proto::FlagCamera)
            {
                const kurenai::proto::Vec3 eye = t.position(mLatest->cameraEye);
                const kurenai::proto::Vec3 look = t.position(mLatest->cameraTarget);
                view = osg::Vec3f(look.x - eye.x, look.y - eye.y, look.z - eye.z);
            }
            view.normalize();
            // The game's pick: the place nearest the middle of its view, now that its camera has turned onto it.
            osg::Vec3f start(view.x(), view.y(), std::sin(0.5f));
            float best = -2.0f;
            for (const kurenai::proto::ProxyTarget& target : mHookTargets)
            {
                const kurenai::proto::Vec3 p = t.position(target.position);
                osg::Vec3f to = osg::Vec3f(p.x, p.y, p.z) - head;
                to.normalize();
                if (to * view > best)
                {
                    best = to * view;
                    start = to;
                }
            }
            mAimYaw = std::atan2(start.x(), start.y());
            mAimPitch = std::atan2(start.z(), std::hypot(start.x(), start.y()));
        }
        if (!aiming && mLatest && (mLatest->flags & kurenai::proto::FlagCamera))
        {
            const kurenai::proto::Vec3 eye = t.position(mLatest->cameraEye);
            const kurenai::proto::Vec3 look = t.position(mLatest->cameraTarget);
            if (std::abs(look.x - eye.x) + std::abs(look.y - eye.y) > 1e-3f)
                mAimYaw = std::atan2(look.x - eye.x, look.y - eye.y);
        }
        if (aiming && mHookAiming)
        {
            constexpr float kTurn = 1.4f; // radians per second at full stick
            const std::uint32_t f = mLatest->flags;
            mAimYaw += ((f & kurenai::proto::FlagRightRight) ? kTurn * dt : 0)
                - ((f & kurenai::proto::FlagRightLeft) ? kTurn * dt : 0);
            mAimPitch += ((f & kurenai::proto::FlagRightUp) ? kTurn * dt : 0)
                - ((f & kurenai::proto::FlagRightDown) ? kTurn * dt : 0);
            mAimPitch = std::clamp(mAimPitch, -0.6f, 1.45f);
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const osg::Vec3f head = mGameFeet + osg::Vec3f(0, 0, 120.0f);
            const osg::Vec3f dir(
                std::sin(mAimYaw) * std::cos(mAimPitch), std::cos(mAimYaw) * std::cos(mAimPitch), std::sin(mAimPitch));
            constexpr int kStatic = MWPhysics::Layers::WORLD | MWPhysics::Layers::HEIGHTMAP;
            const MWPhysics::RayCastingResult hit = world->getRayCasting()->castRay(
                head, head + dir * (kGameReach * t.scale), { world->getPlayerPtr() }, {}, kStatic);
            if (hit.mHit)
            {
                const osg::Vec3f at = hit.mHitPos + hit.mHitNormal * 10.0f;
                if (!mAimHit || (at - mAimAt).length() > 100.0f)
                    Log(Debug::Info) << "Kurenai: hook aim (yaw " << mAimYaw << ", pitch " << mAimPitch << ") meets "
                                     << at.x() << "," << at.y() << "," << at.z() << ", " << (at - head).length()
                                     << " away";
                mAimAt = at;
                mAimHit = true;
            }
            // Pointing at nothing in reach: the last surface it met stays the place.
            mHookTargets.clear();
            if (mAimHit)
            {
                kurenai::proto::ProxyTarget target;
                target.actorId = 0x41494D00u; // the same id throughout: the game keeps the same stand-in and moves it
                target.position = t.toRedNinja({ mAimAt.x(), mAimAt.y(), mAimAt.z() });
                mHookTargets.push_back(target);
            }
            mHookMisses.clear();
            mSinceHookScan = 0;
            kurenai::proto::HookTargets msg;
            msg.targets = mHookTargets;
            send(msg);
            return;
        }
        if (mHookAiming && mAimHit)
            Log(Debug::Info) << "Kurenai: hook aim let go at " << mAimAt.x() << "," << mAimAt.y() << "," << mAimAt.z();
        mHookAiming = false;
        mSinceHookScan += dt;
        if (mSinceHookScan >= 0.25f)
        {
            mSinceHookScan = 0;
            ++mHookScans;
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::Ptr player = world->getPlayerPtr();
            const osg::Vec3f feet = mGameFeet;
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
                    osg::Vec3f at = top.mHitPos + osg::Vec3f(0, 0, 10.0f);
                    if ((at - feet).length() > reach)
                        continue;
                    // She must see it: nothing in the way but the thing it is on (from below, a roof's edge or a
                    // branch hides its own top).
                    const MWPhysics::RayCastingResult sight
                        = world->getRayCasting()->castRay(head, at + osg::Vec3f(0, 0, 40.0f), { player }, {}, kStatic);
                    if (sight.mHit && (sight.mHitPos - at).length() > 250.0f)
                        continue;
                    // Hidden by its own edge: the place is that edge, where her line meets it. A top behind the edge
                    // is out of the game's reach, the line to it going through the wall under the edge.
                    if (sight.mHit)
                    {
                        osg::Vec3f back = head - sight.mHitPos;
                        back.normalize();
                        at = sight.mHitPos + back * 10.0f;
                    }
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

    void Driver::startSmoke(const osg::Vec3f& at, bool landed)
    {
        Smoke s;
        s.effectId = "kurenai_smoke_" + std::to_string(++mSmokeBombs);
        s.position = at;
        if (landed)
            s.age = kSmokeDelay; // a thrown bomb's cloud starts as it lands
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
        mSmokeFlight.clear();
        mSmokeButton = 0;
        mSmokeTap = 0;
        if (mSmokeBombNode)
            mSmokeBombNode->setNodeMask(0u);
        if (mSmokeAimNode)
            mSmokeAimNode->setNodeMask(0u);
    }

    void Driver::smokeArc(const MWWorld::Ptr& player, std::vector<osg::Vec3f>& arc)
    {
        // From her shoulder, along her heading at mSmokeElev, under gravity, to the first wall, ground, actor or water.
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const ESM::Position& pos = player.getRefData().getPosition();
        const osg::Vec3f forward(std::sin(pos.rot[2]), std::cos(pos.rot[2]), 0);
        osg::Vec3f p = pos.asVec3() + osg::Vec3f(0, 0, world->getHalfExtents(player).z() * 1.7f) + forward * 20.0f;
        osg::Vec3f v = (forward * std::cos(mSmokeElev) + osg::Vec3f(0, 0, std::sin(mSmokeElev))) * kSmokeSpeed;
        const MWWorld::CellStore* cell = player.isInCell() ? player.getCell() : nullptr;
        const bool water = cell && cell->getCell()->hasWater();
        const float waterLevel = water ? cell->getWaterLevel() : 0;
        arc.assign(1, p);
        for (float t = 0; t < kSmokeMaxTime; t += kSmokeStep)
        {
            const osg::Vec3f next = p + v * kSmokeStep + osg::Vec3f(0, 0, -0.5f * kSmokeGravity * kSmokeStep * kSmokeStep);
            v.z() -= kSmokeGravity * kSmokeStep;
            MWPhysics::RayCastingResult hit;
            {
                std::lock_guard<std::timed_mutex> lock(mWorldMutex);
                hit = world->getRayCasting()->castRay(p, next, { player }, {},
                    MWPhysics::Layers::WORLD | MWPhysics::Layers::HEIGHTMAP | MWPhysics::Layers::DOOR
                        | MWPhysics::Layers::ACTOR);
            }
            if (hit.mHit)
            {
                arc.push_back(hit.mHitPos);
                return;
            }
            if (water && p.z() >= waterLevel && next.z() < waterLevel)
            {
                const float f = (p.z() - waterLevel) / (p.z() - next.z());
                arc.push_back(p + (next - p) * f);
                return;
            }
            arc.push_back(next);
            p = next;
        }
    }

    void Driver::updateSmokeAim(const MWWorld::Ptr& player, float dt)
    {
        if (!mSmokeButton || !mLatest)
        {
            if (mSmokeAimNode)
                mSmokeAimNode->setNodeMask(0u);
            return;
        }
        // The mouse (down lowers the throw) or the right stick (up raises it) sets the elevation; she throws along
        // her heading, which the left stick turns.
        constexpr float kTurn = 1.2f; // radians per second
        mSmokeElev -= mLookPitch;
        if (mLatest->flags & kurenai::proto::FlagRightUp)
            mSmokeElev += kTurn * dt;
        if (mLatest->flags & kurenai::proto::FlagRightDown)
            mSmokeElev -= kTurn * dt;
        mSmokeElev = std::clamp(mSmokeElev, -0.35f, 1.25f);
        smokeArc(player, mSmokeArc);

        if (!mSmokeAimNode)
        {
            mSmokeAimNode = new osg::Group;
            mSmokeAimNode->setName("krn_smoke_aim");
            const auto line = [](const osg::Vec4f& colour) {
                osg::ref_ptr<osg::Geometry> g = new osg::Geometry;
                g->setDataVariance(osg::Object::DYNAMIC); // rebuilt every frame
                g->setUseDisplayList(false);
                g->setVertexArray(new osg::Vec3Array);
                osg::ref_ptr<osg::Vec4Array> c = new osg::Vec4Array(1);
                (*c)[0] = colour;
                g->setColorArray(c, osg::Array::BIND_OVERALL);
                return g;
            };
            mSmokeArcLine = line(osg::Vec4f(1.0f, 0.85f, 0.45f, 1.0f));
            mSmokeRing = line(osg::Vec4f(1.0f, 0.25f, 0.2f, 1.0f));
            mSmokeAimNode->addChild(mSmokeArcLine);
            mSmokeAimNode->addChild(mSmokeRing);
            // Seen through walls and her body, as an aiming guide.
            osg::StateSet* ss = mSmokeAimNode->getOrCreateStateSet();
            ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
            ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
            ss->setAttributeAndModes(new osg::LineWidth(3.0f));
            ss->setRenderBinDetails(1000, "RenderBin");
            MWBase::Environment::get().getWorld()->getRenderingManager()->getLightRoot()->addChild(mSmokeAimNode);

            // The bomb in flight: a small dark ball.
            mSmokeBombNode = new osg::MatrixTransform;
            mSmokeBombNode->setName("krn_smoke_bomb");
            mSmokeBombNode->setDataVariance(osg::Object::DYNAMIC);
            osg::ref_ptr<osg::Geometry> ball = line(osg::Vec4f(0.15f, 0.15f, 0.17f, 1.0f));
            osg::ref_ptr<osg::Vec3Array> bv = new osg::Vec3Array;
            for (int i = 0; i < 3; ++i) // three crossed rings
                for (int k = 0; k < 12; ++k)
                {
                    const float a = osg::PI * 2 * k / 12, r = 6.0f;
                    const osg::Vec3f q(r * std::cos(a), r * std::sin(a), 0);
                    bv->push_back(i == 0 ? q : i == 1 ? osg::Vec3f(q.x(), 0, q.y()) : osg::Vec3f(0, q.x(), q.y()));
                }
            ball->setVertexArray(bv);
            for (int i = 0; i < 3; ++i)
                ball->addPrimitiveSet(new osg::DrawArrays(GL_LINE_LOOP, i * 12, 12));
            ball->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
            ball->getOrCreateStateSet()->setAttributeAndModes(new osg::LineWidth(4.0f));
            mSmokeBombNode->addChild(ball);
            mSmokeBombNode->setNodeMask(0u);
            MWBase::Environment::get().getWorld()->getRenderingManager()->getLightRoot()->addChild(mSmokeBombNode);
            Log(Debug::Info) << "Kurenai: smoke bomb aim shown";
        }
        osg::ref_ptr<osg::Vec3Array> arc = new osg::Vec3Array(mSmokeArc.begin(), mSmokeArc.end());
        const GLsizei arcCount = static_cast<GLsizei>(arc->size());
        // Cross ticks every 0.1 s of flight: they shrink with distance, so the arc reads as going away from the
        // camera even when the camera is straight behind her.
        const float yaw = player.getRefData().getPosition().rot[2];
        const osg::Vec3f side(std::cos(yaw) * 8.0f, -std::sin(yaw) * 8.0f, 0);
        for (GLsizei i = 3; i < arcCount; i += 3)
        {
            const osg::Vec3f p = mSmokeArc[i];
            arc->push_back(p - side);
            arc->push_back(p + side);
        }
        mSmokeArcLine->setVertexArray(arc);
        mSmokeArcLine->removePrimitiveSet(0, mSmokeArcLine->getNumPrimitiveSets());
        mSmokeArcLine->addPrimitiveSet(new osg::DrawArrays(GL_LINE_STRIP, 0, arcCount));
        if (arc->size() > static_cast<std::size_t>(arcCount))
            mSmokeArcLine->addPrimitiveSet(
                new osg::DrawArrays(GL_LINES, arcCount, static_cast<GLsizei>(arc->size()) - arcCount));
        // Where it lands: a small ring, and the cloud's reach around it.
        const osg::Vec3f land = mSmokeArc.back() + osg::Vec3f(0, 0, 4);
        osg::ref_ptr<osg::Vec3Array> ring = new osg::Vec3Array;
        constexpr int kSides = 40;
        for (const float r : { 25.0f, mSmokeRadius })
            for (int k = 0; k < kSides; ++k)
            {
                const float a = osg::PI * 2 * k / kSides;
                ring->push_back(land + osg::Vec3f(r * std::cos(a), r * std::sin(a), 0));
            }
        mSmokeRing->setVertexArray(ring);
        mSmokeRing->removePrimitiveSet(0, mSmokeRing->getNumPrimitiveSets());
        mSmokeRing->addPrimitiveSet(new osg::DrawArrays(GL_LINE_LOOP, 0, kSides));
        mSmokeRing->addPrimitiveSet(new osg::DrawArrays(GL_LINE_LOOP, kSides, kSides));
        mSmokeAimNode->setNodeMask(MWRender::Mask_Effect);
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

    void Driver::pollRecoverKey()
    {
        if (mRecoverScancode == 0)
            return;
        int count = 0;
        const Uint8* keys = SDL_GetKeyboardState(&count);
        const bool down = mRecoverScancode < count && keys[mRecoverScancode];
        const bool pressed = down && !mRecoverHeld;
        mRecoverHeld = down;
        if (!pressed || !mControlling || MWBase::Environment::get().getWindowManager()->isGuiMode())
            return;
        mRecoverNow = true;
    }

    void Driver::pollControlsKey()
    {
        if (mControlsScancode == 0)
            return;
        int count = 0;
        const Uint8* keys = SDL_GetKeyboardState(&count);
        const bool down = mControlsScancode < count && keys[mControlsScancode];
        const bool pressed = down && !mControlsHeld;
        mControlsHeld = down;
        if (!pressed || !mControlling)
            return;
        MWBase::WindowManager* wm = MWBase::Environment::get().getWindowManager();
        // F1 is also Morrowind's quick keys menu, opened by this same press: in combat mode the card replaces it.
        if (wm->containsMode(MWGui::GM_QuickKeysMenu))
            wm->removeGuiMode(MWGui::GM_QuickKeysMenu);
        if (!wm->isGuiMode())
            showControls();
    }

    void Driver::showControls()
    {
        const MWBase::InputManager* input = MWBase::Environment::get().getInputManager();
        const auto bound = [&](int action) {
            std::string key = input->getActionKeyBindingName(action);
            const std::string pad = input->getActionControllerBindingName(action);
            if (!pad.empty() && pad != key)
                key += key.empty() ? pad : " / " + pad;
            return key.empty() ? std::string("(not bound)") : key;
        };
        std::ostringstream card;
        card << "Kurenai's controls (change them in Options > Controls)\n\n"
             << "Move: " << bound(MWInput::A_MoveForward) << ", " << bound(MWInput::A_MoveLeft) << ", "
             << bound(MWInput::A_MoveBackward) << ", " << bound(MWInput::A_MoveRight) << "\n"
             << "Jump (Cross): " << bound(MWInput::A_Jump) << "\n"
             << "Attack (Square): " << bound(MWInput::A_Use) << "\n"
             << "Wire / smoke bomb (Circle): " << bound(MWInput::A_Activate) << "\n"
             << "Hook / action (Triangle, hold to aim): " << bound(MWInput::A_ToggleWeapon) << "\n"
             << "Lock-on (R1): " << bound(MWInput::A_Run) << "\n"
             << "Block (L1): " << bound(MWInput::A_ToggleSpell) << "\n"
             << "Crouch, wall hug next to a wall (L2): " << bound(MWInput::A_Sneak) << "\n"
             << "Items / pause (Start): " << bound(MWInput::A_GameMenu) << "\n"
             << "Item menu (Select): " << Settings::kurenai().mSelectKey.get() << " / pad Back\n"
             << "Camera / zoom (right stick; with the telescope worn and R2 held, up and down zoom): arrow keys\n"
             << "First person view, R2 (hold): "
             << bound(MWInput::A_TogglePOV) << "\n"
             << "Tetsugen blade (D-pad Left): " << bound(MWInput::A_CycleWeaponLeft) << "\n"
             << "Tetsugen fundo / weight (D-pad Right): " << bound(MWInput::A_CycleWeaponRight) << "\n"
             << "Back onto solid ground (if she ends up in the void): " << Settings::kurenai().mRecoverKey.get() << "\n\n"
             << Settings::kurenai().mModeToggleKey.get() << ": back to your Morrowind character";
        MWBase::Environment::get().getWindowManager()->interactiveMessageBox(card.str(), { "OK" });
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
        // Her tetsugen's attachments: D-pad Left the blade, D-pad Right the fundo (weight). On the keyboard they are
        // OpenMW's previous / next weapon keys ([ and ], rebindable in Options > Controls); on a pad, its D-pad.
        if (held(MWInput::A_CycleWeaponLeft))
            in.buttons |= kurenai::proto::BtnLeft;
        if (held(MWInput::A_CycleWeaponRight))
            in.buttons |= kurenai::proto::BtnRight;
        // The right stick (camera; with R2 and the telescope worn, zoom) from the keyboard's arrow keys; a pad's own
        // right stick reaches the game through the player's PCSX2 pad.
        {
            int count = 0;
            const Uint8* keys = SDL_GetKeyboardState(&count);
            const auto down = [&](SDL_Scancode k) { return k < count && keys[k]; };
            const float rx = (down(SDL_SCANCODE_RIGHT) ? 1.0f : 0.0f) - (down(SDL_SCANCODE_LEFT) ? 1.0f : 0.0f);
            const float ry = (down(SDL_SCANCODE_DOWN) ? 1.0f : 0.0f) - (down(SDL_SCANCODE_UP) ? 1.0f : 0.0f);
            in.rx = axisByte(rx);
            in.ry = axisByte(ry); // the pad's Y grows downwards
        }
        // Joysticks SDL has no standard layout for (a gaming mouse's side pad, an arcade stick ...): button by button, as the
        // 'joystick map' setting says; a hat is the D-pad, axes 0 / 1 the left stick, 2 / 3 the right stick.
        mJoyWeaponNow = false;
        for (int j = 0; j < SDL_NumJoysticks(); ++j)
        {
            if (SDL_IsGameController(j))
                continue;
            SDL_Joystick*& js = mRawPads[j];
            if (!js)
            {
                js = SDL_JoystickOpen(j);
                if (!js)
                    continue;
                Log(Debug::Info) << "Kurenai: joystick " << j << " '" << SDL_JoystickName(js) << "': " << SDL_JoystickNumButtons(js)
                                 << " buttons, " << SDL_JoystickNumAxes(js) << " axes, " << SDL_JoystickNumHats(js)
                                 << " hats (read button by button: the 'joystick map' setting)";
            }
            if (!SDL_JoystickGetAttached(js))
            {
                SDL_JoystickClose(js);
                js = nullptr;
                continue;
            }
            const auto act = [&](const std::string& a) {
                using namespace kurenai::proto;
                if (a == "cross") in.buttons |= BtnCross;
                else if (a == "circle") in.buttons |= BtnCircle;
                else if (a == "square") in.buttons |= BtnSquare;
                else if (a == "triangle") in.buttons |= BtnTriangle;
                else if (a == "l1") in.buttons |= BtnL1;
                else if (a == "l2") in.buttons |= BtnL2;
                else if (a == "l3") in.buttons |= BtnL3;
                else if (a == "r1") in.buttons |= BtnR1;
                else if (a == "r2") in.buttons |= BtnR2;
                else if (a == "r3") in.buttons |= BtnR3;
                else if (a == "select") in.buttons |= BtnSelect;
                else if (a == "start") in.buttons |= BtnStart;
                else if (a == "up") in.buttons |= BtnUp;
                else if (a == "down") in.buttons |= BtnDown;
                else if (a == "left") in.buttons |= BtnLeft;
                else if (a == "right") in.buttons |= BtnRight;
                else if (a == "weapon") mJoyWeaponNow = true;
            };
            for (int b = 0; b < SDL_JoystickNumButtons(js) && b < 64; ++b)
            {
                if (!SDL_JoystickGetButton(js, b))
                    continue;
                const std::string action = static_cast<std::size_t>(b) < mJoyMap.size() ? mJoyMap[b] : "-";
                if (mJoySeen.insert(j * 256 + b).second)
                    Log(Debug::Info) << "Kurenai: joystick " << j << " button " << b << " pressed (" << (action == "-" ? "no action" : action) << ")";
                act(action);
            }
            for (int h = 0; h < SDL_JoystickNumHats(js); ++h)
            {
                const Uint8 hat = SDL_JoystickGetHat(js, h);
                if (hat & SDL_HAT_UP) act("up");
                if (hat & SDL_HAT_DOWN) act("down");
                if (hat & SDL_HAT_LEFT) act("left");
                if (hat & SDL_HAT_RIGHT) act("right");
            }
            const int axes = SDL_JoystickNumAxes(js);
            const auto axis = [&](int a) { return a < axes ? SDL_JoystickGetAxis(js, a) / 32767.0f : 0.0f; };
            if (std::hypot(axis(0), axis(1)) > 0.25f)
            {
                in.lx = axisByte(axis(0));
                in.ly = axisByte(axis(1)); // the pad's Y grows downwards, as SDL's
            }
            if (axes >= 4 && std::hypot(axis(2), axis(3)) > 0.25f)
            {
                in.rx = axisByte(axis(2));
                in.ry = axisByte(axis(3));
            }
        }
        // Select (the item menu) and R2 (first person view; with the telescope worn, the right stick zooms): the
        // keyboard's select key and Morrowind's first-person key, or the pad's Back button and right trigger.
        if (mSelectScancode != 0)
        {
            int count = 0;
            const Uint8* keys = SDL_GetKeyboardState(&count);
            if (mSelectScancode < count && keys[mSelectScancode])
                in.buttons |= kurenai::proto::BtnSelect;
        }
        if (held(MWInput::A_TogglePOV))
            in.buttons |= kurenai::proto::BtnR2;
        for (int i = 0; i < 4; ++i)
            if (SDL_GameController* pad = SDL_GameControllerFromPlayerIndex(i))
            {
                if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK))
                    in.buttons |= kurenai::proto::BtnSelect;
                if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000)
                    in.buttons |= kurenai::proto::BtnR2;
                if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP))
                    in.buttons |= kurenai::proto::BtnUp;
                if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
                    in.buttons |= kurenai::proto::BtnDown;
                if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
                    in.buttons |= kurenai::proto::BtnLeft;
                if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
                    in.buttons |= kurenai::proto::BtnRight;
            }
        // D-pad Left / Right (the keyboard's [ and ]) go to the game: it switches her tetsugen between the Blade and the
        // Fundo itself (PCSX2 keeps both unlocked), plays its own Fundo moves, and says which head she carries
        // (FlagFundo). The engine only follows that, below.
        {
            const bool left = (in.buttons & kurenai::proto::BtnLeft) != 0, right = (in.buttons & kurenai::proto::BtnRight) != 0;
            if (left != mAttachPrevLeft || right != mAttachPrevRight)
                Log(Debug::Info) << "Kurenai: D-pad left " << left << " right " << right << " sent to the game";
            mAttachPrevLeft = left;
            mAttachPrevRight = right;
        }
        // Her smoke bomb's button: held, it aims (updateSmokeAim) and the game doesn't see it; let go, it is tapped.
        std::uint16_t smokeButton = 0;
        if (mLatest && !mHudMenuUp)
        {
            if ((mLatest->flags & kurenai::proto::FlagSmokeTriangle) && held(MWInput::A_ToggleWeapon))
                smokeButton = kurenai::proto::BtnTriangle;
            else if ((mLatest->flags & kurenai::proto::FlagSmokeCircle) && held(MWInput::A_Activate))
                smokeButton = kurenai::proto::BtnCircle;
        }
        if (smokeButton)
            in.buttons &= static_cast<std::uint16_t>(~smokeButton);
        else if (mSmokeButton)
        {
            mSmokeTapButton = mSmokeButton;
            mSmokeTap = kSmokeTapTime;
        }
        mSmokeButton = smokeButton;
        if (mSmokeTap > 0)
            in.buttons |= mSmokeTapButton;
        // Below the surface Jump swims up (updateSwim) and her breathing tube stays put: the game sees neither.
        if (mLatest && swimMotions().count(mLatest->animId) && mSwimDepth.load() >= kSurfaceDepth)
            in.buttons &= static_cast<std::uint16_t>(~(kurenai::proto::BtnCross | kurenai::proto::BtnL2));

        // The weapon screen: its key opens it; Up / Down (or W / S) choose a part, Left / Right (A / D) its version. The game
        // gets nothing while it is open.
        {
            int count = 0;
            const Uint8* keys = SDL_GetKeyboardState(&count);
            const auto down = [&](SDL_Scancode k) { return k < count && keys[k]; };
            const bool now[5] = { (mWeaponScancode != 0 && down(static_cast<SDL_Scancode>(mWeaponScancode))) || mJoyWeaponNow,
                down(SDL_SCANCODE_UP) || down(SDL_SCANCODE_W), down(SDL_SCANCODE_DOWN) || down(SDL_SCANCODE_S),
                down(SDL_SCANCODE_LEFT) || down(SDL_SCANCODE_A), down(SDL_SCANCODE_RIGHT) || down(SDL_SCANCODE_D) };
            const auto edge = [&](int i) { return now[i] && !mWeaponPrev[i]; };
            if (edge(0) && mSinceUi < 3.0f && mHdUi)
            {
                mWeaponOpen = !mWeaponOpen;
                if (mWeaponOpen)
                {
                    mWeaponLevels = mUi.weaponLevels;
                    mWeaponRow = 0;
                }
            }
            else if (mWeaponOpen)
            {
                if (edge(1))
                    mWeaponRow = (mWeaponRow + 3) % 4;
                if (edge(2))
                    mWeaponRow = (mWeaponRow + 1) % 4;
                int step = 0;
                if (edge(3))
                    step = -1;
                if (edge(4))
                    step = 1;
                if (step != 0)
                {
                    const int level = std::clamp<int>(mWeaponLevels[mWeaponRow] + step, 0, 2);
                    if (level != mWeaponLevels[mWeaponRow])
                    {
                        mWeaponLevels[mWeaponRow] = static_cast<std::uint8_t>(level);
                        kurenai::proto::WeaponLevels m;
                        m.levels = mWeaponLevels;
                        send(m);
                        Log(Debug::Info) << "Kurenai: weapon screen: part " << mWeaponRow << " -> version " << level + 1;
                    }
                }
            }
            for (int i = 0; i < 5; ++i)
                mWeaponPrev[i] = now[i];
            if (mWeaponOpen)
            {
                in = kurenai::proto::Input{};
                in.lx = axisByte(0.0f);
                in.ly = axisByte(0.0f);
                in.rx = axisByte(0.0f);
                in.ry = axisByte(0.0f);
            }
        }
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
        mSmokeTap = std::max(0.0f, mSmokeTap - dt);
        const std::uint64_t now = kurenai::monotonicNs();
        mLink->tick(now);
        mFrameBeatNs.store(now, std::memory_order_relaxed);

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
                const int attachment = (ps->flags & kurenai::proto::FlagFundo) ? 1 : 0;
                if (attachment != mAttachment)
                {
                    Log(Debug::Info) << "Kurenai: tetsugen " << (attachment ? "fundo" : "blade") << " (the game's choice)";
                    mAttachment = attachment;
                    if (mNative)
                        mNative->setAttachment(mAttachment);
                    MWBase::Environment::get().getWindowManager()->messageBox(
                        mAttachment == 1 ? "Tetsugen: fundo (weight)" : "Tetsugen: blade");
                }
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
            else if (const auto* ui = std::get_if<kurenai::proto::UiState>(&m.payload))
            {
                mUi = *ui;
                mSinceUi = 0;
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
        updateKillFx(dt);
        if (mHud)
        {
            mHud->beat(now);
            updateHud(dt);
        }
        updateHdUi(dt);
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
        if (mPeerAlive && mHealthMode == "morrowind")
            sendMorrowindHealth();
        pollModeKey();
        pollControlsKey();
        pollRecoverKey();
        if (!mToldControls && mControlling && mControlsScancode != 0
            && !MWBase::Environment::get().getWindowManager()->isGuiMode())
        {
            mToldControls = true;
            MWBase::Environment::get().getWindowManager()->messageBox(
                "Press " + Settings::kurenai().mControlsKey.get() + " for Kurenai's controls");
        }
        // Morrowind mode: the player stays the player until the toggle key makes her Kurenai; say so once.
        if (!mToldToggle && mPeerAlive && mMorrowindMode && mToggleScancode != 0
            && !MWBase::Environment::get().getWindowManager()->isGuiMode())
        {
            mToldToggle = true;
            MWBase::Environment::get().getWindowManager()->messageBox(
                "Red Ninja is ready: press " + Settings::kurenai().mModeToggleKey.get() + " to become Kurenai");
        }
        const bool drive = mPeerAlive && !mMorrowindMode;
        if (drive && !mControlling)
            takeControl();
        else if (!drive && mControlling)
            releaseControl(mPeerAlive ? "Morrowind mode" : "PCSX2 stopped responding");
        mControlling = drive;
        if (mPs2)
            mPs2->setDriven(mControlling && !MWBase::Environment::get().getWindowManager()->isGuiMode());
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
        if (e.kind & kurenai::kCombatDart)
        {
            handleDart(e, p0, p1);
            return;
        }
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
            // Every living actor takes her blade here, proxies too: PCSX2 puts a proxy's health back during her
            // attacks, so the game's own blade on it passes nothing on twice.
            if (std::find(mHitThisAttack.begin(), mHitThisAttack.end(), actor.getBase()) != mHitThisAttack.end())
                continue;
            // The actor as an upright capsule: its collision half extents around its feet position.
            const osg::Vec3f half = world->getHalfExtents(actor);
            const osg::Vec3f feet = actor.getRefData().getPosition().asVec3();
            osg::Vec3f hitPos;
            // At least as tall as her blade swings over the ground: a rat or a mudcrab is struck as in Morrowind, where
            // a swing hits whatever is in front, not only what the blade's line passes through.
            const float top = std::max(2 * half.z(), 140.0f * t.scale);
            const float d = segmentDistance(p0, p1, feet, feet + osg::Vec3f(0, 0, top), hitPos);
            if (d > radius + std::max(half.x(), half.y()))
                continue;
            mHitThisAttack.push_back(actor.getBase());
            ++mCombatHits;
            // Her stealth kill on an actor the game has no guard for (a stage without enemies, or more actors than
            // guards): struck from behind while it is not fighting, it dies at once. Proxied actors get the game's own
            // kill through their guard. Not the game's kill cinematic: that needs a stage guard to play on.
            float damage = e.damage * (mAttachment == 1 ? 1.6f : 1.0f); // the fundo (weight) hits harder than the blade
            bool stealthKill = false;
            MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            const float healthBefore = stats.getHealth().getCurrent();
            if (!proxied(actor) && !stats.getAiSequence().isInCombat())
            {
                const float yaw = actor.getRefData().getPosition().rot[2];
                const osg::Vec3f facing(std::sin(yaw), std::cos(yaw), 0);
                osg::Vec3f toHer = player.getRefData().getPosition().asVec3() - feet;
                toHer.z() = 0;
                if (toHer.normalize() > 0 && facing * toHer < -0.3f)
                {
                    damage = std::max(damage, stats.getHealth().getCurrent() + 1.0f);
                    stealthKill = true;
                    ++mStealthKills;
                    Log(Debug::Info) << "Kurenai: stealth kill on " << actor.getCellRef().getRefId()
                                     << " (no stage guard for it): struck from behind, unaware";
                }
            }
            // Morrowind's own melee pipeline (Lua combat): damage, hit reaction, AI aggression, sounds.
            MWBase::Environment::get().getLuaManager()->onHit(player, actor, MWWorld::Ptr(), MWWorld::Ptr(), 0, 1.0f,
                damage, true, hitPos, true, MWMechanics::DamageSourceType::Melee);
            if (stealthKill)
            {
                // Morrowind's combat script scales hit damage (armor, difficulty); her kill is a kill here.
                MWMechanics::DynamicStat<float> health = stats.getHealth();
                health.setCurrent(0);
                stats.setHealth(health);
            }
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
                             << ") hits " << actor.getCellRef().getRefId() << " for " << damage
                             << " damage (health before " << healthBefore << ")";
        }
    }

    bool Driver::proxied(const MWWorld::Ptr& actor) const
    {
        if (!mProxyEnabled)
            return false;
        for (const auto& entry : mProxies)
            if (entry.second.actor == actor && entry.second.assigned)
                return true;
        return false;
    }

    void Driver::handleDart(const kurenai::proto::CombatEvent& e, const osg::Vec3f& from, const osg::Vec3f& to)
    {
        // Her dart flies along her aim (the game camera's line) to the first actor or wall on it. The game's darts hit
        // at once, so this does too. An actor with a proxy took the game's own dart through it (ProxyStatus).
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world->getPlayerPtr();
        const MWPhysics::RayCastingResult hit = world->getRayCasting()->castRay(from, to, { player }, {},
            MWPhysics::Layers::WORLD | MWPhysics::Layers::HEIGHTMAP | MWPhysics::Layers::DOOR
                | MWPhysics::Layers::ACTOR);
        const MWWorld::Ptr actor = hit.mHit ? hit.mHitObject : MWWorld::Ptr();
        if (actor.isEmpty() || !actor.getClass().isActor() || actor.getClass().getCreatureStats(actor).isDead())
        {
            Log(Debug::Info) << "Kurenai: dart " << (e.attackId & 0xffff) << " from (" << from.x() << ", " << from.y()
                             << ", " << from.z() << ") toward (" << to.x() << ", " << to.y() << ", " << to.z()
                             << ") hits no actor" << (hit.mHit ? " (a wall first)" : "");
            if (hit.mHit)
            {
                const osg::Vec3f pp = player.getRefData().getPosition().asVec3();
                Log(Debug::Info) << "Kurenai: dart wall at (" << hit.mHitPos.x() << ", " << hit.mHitPos.y() << ", "
                                 << hit.mHitPos.z() << "), player at (" << pp.x() << ", " << pp.y() << ", " << pp.z()
                                 << ")"
                                 << (hit.mHitObject.isEmpty()
                                            ? ""
                                            : " object " + hit.mHitObject.getCellRef().getRefId().toDebugString());
            }
            return;
        }
        if (proxied(actor))
        {
            Log(Debug::Info) << "Kurenai: dart " << (e.attackId & 0xffff) << " on " << actor.getCellRef().getRefId()
                             << ": its proxy takes the game's dart";
            return;
        }
        MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
        const float damage = std::max(1.0f, e.damage * stats.getHealth().getModified());
        ++mCombatHits;
        MWBase::Environment::get().getLuaManager()->onHit(player, actor, MWWorld::Ptr(), MWWorld::Ptr(), 0, 1.0f,
            damage, true, hit.mHitPos, true, MWMechanics::DamageSourceType::Ranged);
        Log(Debug::Info) << "Kurenai: dart " << (e.attackId & 0xffff) << " hits " << actor.getCellRef().getRefId()
                         << " for " << damage << " damage (health before " << stats.getHealth().getCurrent() << ")";
    }

    void Driver::sendProxyTargets(const kurenai::CoordTransform& t)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world->getPlayerPtr();
        const osg::Vec3f here = mGameFeet; // where the game has her (not where she is shown ahead of it)
        std::vector<MWWorld::Ptr> actors;
        MWBase::Environment::get().getMechanicsManager()->getActorsInRange(here, mProxyRadius, actors);
        std::vector<std::pair<float, MWWorld::Ptr>> near;
        for (const MWWorld::Ptr& a : actors)
            if (a != player && a.getRefData().isEnabled() && !a.getClass().getCreatureStats(a).isDead())
                near.emplace_back((a.getRefData().getPosition().asVec3() - here).length2(), a);
        // The actor she is locked on first, so the game's own lock-on, stealth kill and darts have it too.
        for (auto& n : near)
            if (n.second == mLockActor)
                n.first = -1;
        std::sort(near.begin(), near.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        if (near.size() > kurenai::proto::kMaxProxies)
            near.resize(kurenai::proto::kMaxProxies);

        kurenai::proto::ProxyTargets msg;
        std::map<std::uint32_t, Proxy> keep;
        for (const auto& [d, actor] : near)
        {
            const std::uint32_t id = actorId(actor);
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

    std::uint32_t Driver::actorId(const MWWorld::Ptr& actor)
    {
        auto [it, isNew] = mProxyIds.emplace(actor.getBase(), mNextProxyId);
        if (isNew)
            ++mNextProxyId;
        return it->second;
    }

    bool Driver::lockable(const MWWorld::Ptr& actor) const
    {
        return !actor.isEmpty() && actor.getRefData().isEnabled() && actor.getRefData().getBaseNode()
            && !actor.getClass().getCreatureStats(actor).isDead();
    }

    MWWorld::Ptr Driver::pickLockTarget() const
    {
        // Any living NPC or creature in front of her (or right next to her) within lock range that she can see; the
        // hostile ones first, then the nearest and most straight ahead. The stage's own enemies play no part.
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world->getPlayerPtr();
        const osg::Vec3f here = player.getRefData().getPosition().asVec3();
        const float yaw = player.getRefData().getPosition().rot[2];
        const osg::Vec3f facing(std::sin(yaw), std::cos(yaw), 0);
        const osg::Vec3f eye = here + osg::Vec3f(0, 0, 2 * world->getHalfExtents(player).z() * 0.85f);
        std::vector<MWWorld::Ptr> actors;
        MWBase::MechanicsManager* mechanics = MWBase::Environment::get().getMechanicsManager();
        mechanics->getActorsInRange(here, mLockRange, actors);
        MWWorld::Ptr best;
        float bestScore = 1e30f;
        for (const MWWorld::Ptr& a : actors)
        {
            if (a == player || !lockable(a))
                continue;
            const osg::Vec3f feet = a.getRefData().getPosition().asVec3();
            osg::Vec3f flat = feet - here;
            flat.z() = 0;
            const float dist = flat.length();
            if (dist > mLockRange)
                continue;
            const float angle = dist > 1 ? std::acos(std::clamp(flat * facing / dist, -1.0f, 1.0f)) : 0.0f;
            const bool hostile = a.getClass().getCreatureStats(a).getAiSequence().isInCombat(player)
                || mechanics->isAggressive(a, player);
            // Ahead of her; one close by from any side (a hostile one from farther: it is coming for her).
            if (angle > osg::DegreesToRadians(75.0f) && dist > (hostile ? 1000.0f : 300.0f))
            {
                Log(Debug::Verbose) << "Kurenai: lock-on skips " << a.getCellRef().getRefId() << " at " << dist << ", "
                                    << osg::RadiansToDegrees(angle) << " deg: behind her";
                continue;
            }
            // Seen if the line from her eyes to its middle or just over its top is clear (a small creature's middle
            // can be hidden by a bank its back shows over).
            const float height = 2 * world->getHalfExtents(a).z();
            bool seen = false;
            for (const float z : { height * 0.6f, height + 10.0f })
                seen = seen
                    || !world->getRayCasting()
                            ->castRay(eye, feet + osg::Vec3f(0, 0, z), { player, a }, {},
                                MWPhysics::Layers::WORLD | MWPhysics::Layers::HEIGHTMAP | MWPhysics::Layers::DOOR)
                            .mHit;
            if (!seen)
            {
                Log(Debug::Verbose) << "Kurenai: lock-on skips " << a.getCellRef().getRefId() << " at " << dist
                                    << ": out of sight";
                continue;
            }
            // Hostile ones first: a quarter of the score, so one coming for her wins over a bystander ahead.
            const float score = dist * (1.0f + angle) * (hostile ? 0.25f : 1.0f);
            Log(Debug::Verbose) << "Kurenai: lock-on candidate " << a.getCellRef().getRefId() << " at " << dist << ", "
                                << osg::RadiansToDegrees(angle) << " deg" << (hostile ? ", hostile" : "") << ": "
                                << score;
            if (score < bestScore)
                bestScore = score, best = a;
        }
        return best;
    }

    void Driver::updateLockOn(const kurenai::CoordTransform& t)
    {
        const bool held = (mLatest->flags & kurenai::proto::FlagLockHeld) != 0;
        const MWWorld::Ptr before = mLockActor;
        bool near = false;
        if (!mLockActor.isEmpty())
        {
            // Still among the actors around her (a cell change can unload it: never touch a stale reference).
            std::vector<MWWorld::Ptr> actors;
            MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
                MWBase::Environment::get().getWorld()->getPlayerPtr().getRefData().getPosition().asVec3(),
                mLockRange * 1.5f, actors);
            near = std::find(actors.begin(), actors.end(), mLockActor) != actors.end();
            if (!near)
                mLockActor = MWWorld::Ptr();
        }
        if (!held)
            mLockActor = MWWorld::Ptr();
        else if (!mLockHeld || !near || !lockable(mLockActor))
        {
            // R1 pressed, or the target died or got away while it is held: the next one (none: she stays unlocked
            // until R1 is pressed again).
            const bool pressed = !mLockHeld;
            if (pressed || !before.isEmpty())
            {
                // The one she had a moment ago is kept while it is still around (R1 flickering or pressed again
                // is not a new choice); only otherwise the best new one.
                const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                bool again = false;
                if (!mLockLast.isEmpty() && now - mLockLastTime < 1.5)
                {
                    std::vector<MWWorld::Ptr> around;
                    MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
                        MWBase::Environment::get().getWorld()->getPlayerPtr().getRefData().getPosition().asVec3(),
                        mLockRange * 1.5f, around);
                    again = std::find(around.begin(), around.end(), mLockLast) != around.end() && lockable(mLockLast);
                }
                mLockActor = again ? mLockLast : pickLockTarget();
            }
            else
                mLockActor = MWWorld::Ptr();
            if (!mLockActor.isEmpty())
                ++mLockPicks;
            else if (pressed)
                ++mLockMisses;
        }
        mLockHeld = held;
        if (!mLockActor.isEmpty())
        {
            mLockLast = mLockActor;
            mLockLastTime = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        if (mLockActor != before)
            Log(Debug::Info) << "Kurenai: lock-on "
                             << (mLockActor.isEmpty() ? std::string("off")
                                                      : "on " + mLockActor.getCellRef().getRefId().toDebugString());
        if (mLockActor.isEmpty())
        {
            if (!before.isEmpty())
                send(kurenai::proto::LockTarget{});
            return;
        }
        kurenai::proto::LockTarget msg;
        msg.actorId = actorId(mLockActor);
        const ESM::Position& pos = mLockActor.getRefData().getPosition();
        msg.position = t.toRedNinja({ pos.pos[0], pos.pos[1], pos.pos[2] });
        send(msg);
    }

    void Driver::updateLockMarker()
    {
        if (!mLockMarker)
        {
            // The game's lock-on mark: four orange chevrons (up, right, down, left) round the target, each with a dark
            // edge, drawn once. u and v run -1..1 over the square; every chevron is the upper one turned.
            constexpr int n = 128;
            MyGUI::RenderManager& rm = MyGUI::RenderManager::getInstance();
            mLockTexture = rm.createTexture("kurenai_lock_marker");
            mLockTexture->createManual(n, n, MyGUI::TextureUsage::Write, MyGUI::PixelFormat::R8G8B8A8);
            const auto chevron = [](float u, float v, float grow) {
                const float a = v + 0.96f + grow; // down from the apex
                const float arm = a;
                return a >= 0.f && a <= 0.26f + grow && std::abs(u) <= arm + grow && std::abs(u) >= arm - 0.11f - grow;
            };
            const auto any = [&](float u, float v, float grow) {
                return chevron(u, v, grow) || chevron(-u, -v, grow) || chevron(v, -u, grow) || chevron(-v, u, grow);
            };
            if (auto* data = static_cast<std::uint32_t*>(mLockTexture->lock(MyGUI::TextureUsage::Write)))
                for (int y = 0; y < n; ++y)
                    for (int x = 0; x < n; ++x)
                    {
                        const float u = (x + 0.5f) / n * 2.f - 1.f;
                        const float v = (y + 0.5f) / n * 2.f - 1.f;
                        data[y * n + x] = any(u, v, 0.f) ? 0xff30a8ffu : any(u, v, 0.045f) ? 0xd0000000u : 0u;
                    }
            mLockTexture->unlock();
            mLockMarker = MyGUI::Gui::getInstance().createWidget<MyGUI::ImageBox>(
                "ImageBox", MyGUI::IntCoord(0, 0, n, n), MyGUI::Align::Default, "HUD");
            mLockMarker->setNeedMouseFocus(false);
            mLockMarker->setImageTexture("kurenai_lock_marker");
            mLockMarker->setVisible(false);
        }
        bool shown = false;
        if (!mLockActor.isEmpty())
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const osg::Vec3f feet = mLockActor.getRefData().getPosition().asVec3();
            const osg::Vec3f chest = feet + osg::Vec3f(0, 0, world->getHalfExtents(mLockActor).z() * 1.2f);
            const osg::Vec2f at = world->getRenderingManager()->getScreenCoords(osg::BoundingBox(chest, chest));
            if (at != osg::Vec2f(0.5f, 0.f)) // off screen
            {
                const MyGUI::IntSize view = MyGUI::RenderManager::getInstance().getViewSize();
                const int size = std::max(48, view.height / 7); // the game's mark spans about a seventh of the screen's height
                mLockMarker->setCoord(static_cast<int>(at.x() * view.width) - size / 2,
                    static_cast<int>(at.y() * view.height) - size / 2, size, size);
                shown = true;
            }
        }
        mLockMarker->setVisible(shown && !mHudMenuUp);
    }

    void Driver::drawRadar(std::uint32_t* data, int pitch, int w, int h)
    {
        // Seen from above with her facing up: a dark disc with a red rim, a white arrow for her, red dots for hostile
        // actors, yellow for the others, a ring around her lock-on target. Colours are 0xAABBGGRR.
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world->getPlayerPtr();
        const osg::Vec3f here = player.getRefData().getPosition().asVec3();
        const float yaw = player.getRefData().getPosition().rot[2];
        const float cx = w / 2.0f, cy = h / 2.0f, radius = std::min(w, h) / 2.0f - 2.0f;
        auto put = [&](int x, int y, std::uint32_t c) {
            if (x >= 0 && y >= 0 && x < w && y < h)
                data[static_cast<std::size_t>(y) * pitch + x] = c;
        };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                if (d <= radius - 2)
                    put(x, y, 0x90100808u);
                else if (d <= radius)
                    put(x, y, 0xe02020a0u);
            }
        auto dot = [&](float x, float y, float r, std::uint32_t c, bool ring) {
            for (int py = static_cast<int>(y - r - 1); py <= static_cast<int>(y + r + 1); ++py)
                for (int px = static_cast<int>(x - r - 1); px <= static_cast<int>(x + r + 1); ++px)
                {
                    const float d = std::hypot(px + 0.5f - x, py + 0.5f - y);
                    if (ring ? (d <= r && d >= r - 1.2f) : d <= r)
                        put(px, py, c);
                }
        };
        std::vector<MWWorld::Ptr> actors;
        MWBase::MechanicsManager* mechanics = MWBase::Environment::get().getMechanicsManager();
        mechanics->getActorsInRange(here, mRadarRange, actors);
        const float k = (radius - 4) / mRadarRange;
        for (const MWWorld::Ptr& a : actors)
        {
            if (a == player || !lockable(a))
                continue;
            const osg::Vec3f d = a.getRefData().getPosition().asVec3() - here;
            const float ahead = d.x() * std::sin(yaw) + d.y() * std::cos(yaw);
            const float right = d.x() * std::cos(yaw) - d.y() * std::sin(yaw);
            if (std::hypot(ahead, right) > mRadarRange)
                continue;
            const bool hostile = a.getClass().getCreatureStats(a).getAiSequence().isInCombat(player)
                || mechanics->isAggressive(a, player);
            const float x = cx + right * k, y = cy - ahead * k;
            dot(x, y, 2.5f, hostile ? 0xff2828ffu : 0xff30e0f0u, false);
            if (a == mLockActor)
                dot(x, y, 5.5f, 0xffffffffu, true);
        }
        // Her: a small arrow pointing up.
        for (int i = 0; i < 6; ++i)
            for (int j = -i / 2; j <= i / 2; ++j)
                put(static_cast<int>(cx) + j, static_cast<int>(cy) - 3 + i, 0xffffffffu);
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
            else if (s.health > p.lastHealth)
                p.lastHealth = s.health; // PCSX2 put its health back (her blade hits through the engine's test)
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
            const kurenai::proto::Quat heading = kurenai::headingOnly(p.rotation); // a thrown body keeps its tilt
            const std::vector<kurenai::AimDirection> dirs = kurenai::aimDirections(p.bones, heading, map->second);
            target->active = true;
            for (std::size_t i = 0; i < dirs.size(); ++i)
            {
                const kurenai::BoneAim& a = map->second.aims[i];
                target->aims.push_back({ a.bone, a.child, osg::Vec3f(dirs[i].dir.x, dirs[i].dir.y, dirs[i].dir.z),
                    a.sideChild, osg::Vec3f(dirs[i].side.x, dirs[i].side.y, dirs[i].side.z) });
            }
            if (const auto root = kurenai::rootOffset(p.bones, p.position, heading, map->second))
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
        // The game's stealth kill (runs lab_sk_ps2frames): a yellow flare and a cross-shaped spark about 10 frames in, then
        // the blood spray about 58 frames in, over the cut-away. Morrowind's effect meshes stand in for them.
        if (victim.fxDone == 0 && victim.poses >= 10)
            spawnKillFx(actor, victim.fxDone = 1);
        else if (victim.fxDone == 1 && victim.poses >= 56)
            spawnKillFx(actor, victim.fxDone = 2);

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

    void Driver::spawnKillFx(const MWWorld::Ptr& actor, int stage)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const osg::Vec3f feet = actor.getRefData().getPosition().asVec3();
        const float height = world->getHalfExtents(actor).z() * 2.0f;
        // Stage 1: the neck. Stage 2: the chest, lower.
        const osg::Vec3f at = feet + osg::Vec3f(0, 0, height * (stage == 1 ? 0.8f : 0.85f));
        const auto add = [&](const char* model, float scale, float life) {
            KillFx fx;
            fx.id = "kurenai_kill_fx_" + std::to_string(mKillFxCount++);
            fx.life = life;
            world->spawnEffect(VFS::Path::Normalized(model), "", at, scale, false, true, fx.id, true);
            mKillFx.push_back(std::move(fx));
        };
        Log(Debug::Info) << "Kurenai: kill effects " << stage << " on " << actor.getCellRef().getRefId() << " at " << at.x()
                         << ", " << at.y() << ", " << at.z();
        if (stage == 1)
        {
            add("meshes/vfx_defaulthit.nif", 2.5f, 0.8f);
            add("meshes/lava_sparks.nif", 1.5f, 0.8f);
        }
        else
        {
            add("meshes/bloodsplat.nif", 3.5f, 3.0f);
            add("meshes/bloodsplat2.nif", 2.5f, 3.0f);
            add("meshes/bloodsplat3.nif", 2.0f, 3.0f);
        }
    }

    void Driver::updateKillFx(float dt)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        for (auto it = mKillFx.begin(); it != mKillFx.end();)
        {
            it->life -= dt;
            if (it->life <= 0)
            {
                world->removeEffect(it->id);
                it = mKillFx.erase(it);
            }
            else
                ++it;
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
        mLatestPose = p;
        if (mPs2Anim == Ps2Anim::Off || !mLatest)
            return;
        // Each model (outfit) has its own bone layout. The model id also changes with words the game rewrites (her
        // kimono showed a dozen ids across stages), so a model no map names gets the map that gives its pose human
        // limbs, once; only if none fits does Morrowind animate her.
        auto it = mSkeletonMaps.find(p.model);
        if (it == mSkeletonMaps.end())
        {
            auto picked = mPickedMaps.find(p.model);
            if (picked == mPickedMaps.end())
                if (const std::optional<std::uint32_t> key = kurenai::pickSkeletonMap(p.bones, mSkeletonMaps))
                {
                    picked = mPickedMaps.emplace(p.model, *key).first;
                    Log(Debug::Info) << "Kurenai: model " << std::hex << p.model << " has the bones of map " << *key
                                     << std::dec;
                }
            it = picked != mPickedMaps.end() ? mSkeletonMaps.find(picked->second) : mSkeletonMaps.find(0);
        }
        if (p.model != mPoseModel)
        {
            mPoseModel = p.model;
            Log(Debug::Info) << "Kurenai: model " << std::hex << p.model << std::dec << " (" << p.bones.size()
                             << " bones): " << (it != mSkeletonMaps.end() ? "skeleton map found" : "no skeleton map yet, "
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
        // Against her facing alone (the actor only turns): a tilt (her lean on a swing of the wire) stays in the bones.
        const std::vector<kurenai::AimDirection> dirs
            = kurenai::aimDirections(p.bones, kurenai::headingOnly(mLatest->rotation), map);
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
        mHudMenuUp = false;
        mGameMenuFull = false;
        for (MyGUI::ImageBox* box : mHudBoxes)
            box->setVisible(false);
        if (mMorrowindHudHidden)
        {
            MWBase::Environment::get().getWindowManager()->setHudVisibility(true);
            mMorrowindHudHidden = false;
        }
    }

    // Asked by the engine while the player is dead (every frame, and when Morrowind's own death animation ends): true while
    // Red Ninja's code drives her and her death sequence (the game's death motion, then its game over) is still going.
    bool Driver::deathDeferred()
    {
        if (!sInstance)
            return false;
        Driver& d = *sInstance;
        if (d.mDeathState == 0)
        {
            if (!d.mControlling || d.mSinceLatest > 1.0f)
                return false;
            d.mDeathState = 1;
            d.mDeathWait = 0;
            Log(Debug::Info) << "Kurenai: her death: the game's death sequence plays first";
        }
        return d.mDeathState == 1;
    }

    void Driver::updateHdUi(float dt)
    {
        sInstance = this;
        if (mDeathState == 2 || mDeathState == 1)
        {
            const MWWorld::Ptr p = MWBase::Environment::get().getWorld()->getPlayerPtr();
            if (!p.getClass().getCreatureStats(p).isDead())
                mDeathState = 0; // loaded a save, or revived
        }
        if (mDeathState == 1)
        {
            mDeathWait += dt;
            // The game stops updating her once its death sequence hands over to its game over: then (or after 8 s) Morrowind's
            // death screen follows.
            if (mSinceLatest > 1.5f || mDeathWait > 6.0f)
            {
                mDeathState = 2;
                Log(Debug::Info) << "Kurenai: her death sequence is over (" << mDeathWait << " s): Morrowind's death screen";
                MWBase::Environment::get().getStateManager()->askLoadRecent();
            }
        }
        mSinceUi += dt;
        if (!mHdUiTried)
        {
            mHdUiTried = true;
            const std::string dir = Settings::kurenai().mHdUi;
            if (!dir.empty())
            {
                mHdUi = std::make_unique<HdUi>();
                if (!mHdUi->load(dir))
                    mHdUi.reset();
            }
        }
        if (!mHdUi)
            return;
        const bool show = mControlling && mSinceUi < 3.0f;
        if (show && !mTestKillDone && Settings::kurenai().mTestKillSeconds > 0)
        {
            mTestKillClock += dt;
            if (mTestKillClock > Settings::kurenai().mTestKillSeconds)
            {
                mTestKillDone = true;
                const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
                MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
                MWMechanics::DynamicStat<float> hp = stats.getHealth();
                hp.setCurrent(0, true);
                stats.setHealth(hp);
                Log(Debug::Info) << "Kurenai: test kill: her Morrowind health is 0";
            }
        }
        if (mNative && mSinceUi < 3.0f)
            mNative->setWeaponLevels(mWeaponOpen ? mWeaponLevels[0] : mUi.weaponLevels[0],
                mWeaponOpen ? mWeaponLevels[2] : mUi.weaponLevels[2]);
        if (!show)
            mWeaponOpen = false;
        if (mWeaponOpen)
        {
            mHdUi->updateWeapons(mWeaponLevels, mWeaponRow, true);
            return;
        }
        mHdUi->updateWeapons(mWeaponLevels, mWeaponRow, false);
        float health = 1;
        if (MWBase::Environment::get().getWorld()->getPlayerPtr().getClass().getCreatureStats(MWBase::Environment::get().getWorld()->getPlayerPtr()).isDead())
            health = 0;
        else if (mLatest && mLatest->healthMax > 0)
            health = std::clamp(static_cast<float>(mLatest->health) / static_cast<float>(mLatest->healthMax), 0.0f, 1.0f);
        // The game's pause flag is also up for its message boxes: only a full-screen menu in the captured frame is the item menu.
        kurenai::proto::UiState ui = mUi;
        ui.menuOpen = ui.menuOpen && mGameMenuFull;
        mHdUi->update(ui, health, show, mAttachment == 1);
        if (show && mHideMorrowindHud && !mMorrowindHudHidden)
        {
            MWBase::Environment::get().getWindowManager()->setHudVisibility(false);
            mMorrowindHudHidden = true;
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
            // A full-screen menu (the item screen, pause, Game Over) owns every pixel while it is up: it is shown
            // whole and alone, without Morrowind's radar or the lock-on marker over it.
            mHudMenuUp = false;
            for (std::size_t i = 0; i < f.regions.size() && !mHudMenuUp; ++i)
            {
                const kurenai::HudRegion& r = f.regions[i];
                if (r.x == 0 && r.y == 0 && r.w >= f.width && r.h >= f.height && !f.owner.empty())
                    mHudMenuUp = f.owner[f.owner.size() / 2] == i + 1 && f.owner.front() == i + 1;
            }
            mGameMenuFull = mHudMenuUp;
            if (mHdUi && mHdUi->menuUp())
                mHudMenuUp = true;
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
                    if (static_cast<int>(i) + 1 == mRadarRegion)
                    {
                        if (!mHudMenuUp)
                            drawRadar(data, tw, r.w, r.h);
                        t.texture->unlock();
                        continue;
                    }
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
                // The ring, bars and item buttons are drawn natively when the hi-res HUD is up (the radar stays).
                if (mHdUi && mSinceUi < 3.0f && static_cast<int>(i) + 1 != mRadarRegion
                    && (i + 1 != 2 || mHdUi->menuUp()))
                {
                    box->setVisible(false);
                    continue;
                }
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
                if (r.x == 0 && r.y == 0 && r.w >= f.width && r.h >= f.height)
                {
                    // A full-screen region always fits the window whole (whatever the HUD scale), centred.
                    const float fit = std::min(
                        view.width / static_cast<float>(f.width), view.height / static_cast<float>(f.height));
                    const int fw = static_cast<int>(std::lround(r.w * fit));
                    const int fh = static_cast<int>(std::lround(r.h * fit));
                    box->setCoord((view.width - fw) / 2, (view.height - fh) / 2, fw, fh);
                }
                else
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
                for (std::size_t i = 0; i < f.regions.size(); ++i)
                    Log(Debug::Info) << "Kurenai: game HUD region " << i + 1 << ": " << f.regions[i].x << "," << f.regions[i].y
                                     << " " << f.regions[i].w << "x" << f.regions[i].h;
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
            // A model no map names (another outfit) uses the map its limbs fit, as her body does.
            const auto picked = mPickedMaps.find(mPoseModel);
            const auto it = mSkeletonMaps.find(picked != mPickedMaps.end() ? picked->second : mPoseModel);
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

    bool Driver::makeKunai()
    {
        mKunaiNode = new osg::MatrixTransform;
        mKunaiNode->setName("krn_thrown_kunai");
        mKunaiNode->setDataVariance(osg::Object::DYNAMIC);
        Resource::ResourceSystem* resources = MWBase::Environment::get().getResourceSystem();
        const VFS::Path::Normalized p("meshes/krn_native/krn_item_1.nif"); // her held kunai (render object 1)
        osg::ref_ptr<osg::Node> mesh;
        if (resources->getVFS()->exists(p))
        {
            try
            {
                mesh = resources->getSceneManager()->getInstance(p);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Error) << "Kurenai: thrown kunai " << p.value() << ": " << e.what();
            }
        }
        if (mesh)
        {
            // Game units; its longest side is the blade, which flies first.
            osg::ComputeBoundsVisitor bounds;
            mesh->accept(bounds);
            const osg::BoundingBox& b = bounds.getBoundingBox();
            osg::Vec3f axis(1, 0, 0);
            if (b.valid())
            {
                const osg::Vec3f size = b._max - b._min;
                axis = size.y() > size.x() && size.y() >= size.z() ? osg::Vec3f(0, 1, 0)
                    : size.z() > size.x()                          ? osg::Vec3f(0, 0, 1)
                                                                   : osg::Vec3f(1, 0, 0);
            }
            mKunaiMesh = osg::Matrixf::translate(b.valid() ? -b.center() : osg::Vec3f())
                * osg::Matrixf::rotate(axis, osg::Vec3f(1, 0, 0));
            mKunaiNode->addChild(mesh);
            Log(Debug::Info) << "Kurenai: thrown kunai drawn with " << p.value();
        }
        else
        {
            // No item meshes (the native body is off): a short dark blade, game units.
            osg::ref_ptr<osg::Geometry> line = new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> v = new osg::Vec3Array;
            v->push_back(osg::Vec3f(-12, 0, 0));
            v->push_back(osg::Vec3f(12, 0, 0));
            line->setVertexArray(v);
            osg::ref_ptr<osg::Vec4Array> colour = new osg::Vec4Array(1);
            (*colour)[0] = osg::Vec4f(0.2f, 0.2f, 0.22f, 1.0f);
            line->setColorArray(colour, osg::Array::BIND_OVERALL);
            line->addPrimitiveSet(new osg::DrawArrays(GL_LINES, 0, 2));
            osg::StateSet* ss = line->getOrCreateStateSet();
            ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
            ss->setAttributeAndModes(new osg::LineWidth(3.0f));
            mKunaiNode->addChild(line);
            Log(Debug::Info) << "Kurenai: thrown kunai drawn as a line (no " << p.value() << ")";
        }
        mKunaiNode->setNodeMask(0u);
        MWBase::Environment::get().getWorld()->getRenderingManager()->getLightRoot()->addChild(mKunaiNode);
        return true;
    }

    void Driver::updateKunai(const kurenai::CoordTransform& t, float dt)
    {
        std::vector<KunaiLaunch> launches;
        {
            std::lock_guard<std::mutex> lock(mKunaiMutex);
            launches.swap(mKunaiNew);
        }
        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (lockable(mLockActor))
        {
            mKunaiTarget = mLockActor.getCellRef().getRefNum();
            mKunaiTargetAge = 0;
        }
        else
            mKunaiTargetAge += dt;
        for (KunaiLaunch l : launches)
        {
            // Locked on, the game aims the kunai up or down at its target, but its target stands where the PS2 stage
            // has it (or, for an actor without a stage enemy, nowhere: the game throws it level). In Morrowind it flies
            // at the locked actor's chest, wherever that is, when the throw heads its way.
            if (mKunaiTarget.isSet() && mKunaiTargetAge < 1.5f)
            {
                const MWWorld::Ptr target = MWBase::Environment::get().getWorldModel()->getPtr(mKunaiTarget);
                if (lockable(target))
                {
                    const osg::Vec3f chest = target.getRefData().getPosition().asVec3()
                        + osg::Vec3f(0, 0, world->getHalfExtents(target).z() * 1.2f);
                    osg::Vec3f thrown = l.to - l.from, toward = chest - l.from;
                    osg::Vec2f flatThrown(thrown.x(), thrown.y()), flatToward(toward.x(), toward.y());
                    if (flatThrown.normalize() > 1e-3f && flatToward.normalize() > 1e-3f
                        && flatThrown * flatToward > std::cos(osg::DegreesToRadians(30.0f)))
                    {
                        toward.normalize();
                        if (mKunaiThrows < 5)
                            Log(Debug::Info) << "Kurenai: thrown kunai aimed at the locked "
                                             << target.getCellRef().getRefId().toDebugString() << "'s chest: pitch "
                                             << osg::RadiansToDegrees(std::asin(thrown.z() / thrown.length()))
                                             << " -> " << osg::RadiansToDegrees(std::asin(toward.z())) << " degrees";
                        l.to = l.from + toward * thrown.length();
                    }
                }
            }
            // Where it ends in Morrowind: the first wall, ground or actor along its line (not her own body, which
            // it starts in). The game flies it straight.
            MWPhysics::RayCastingResult hit;
            {
                std::lock_guard<std::timed_mutex> lock(mWorldMutex);
                hit = world->getRayCasting()->castRay(l.from, l.to, { world->getPlayerPtr() }, {},
                    MWPhysics::Layers::WORLD | MWPhysics::Layers::HEIGHTMAP | MWPhysics::Layers::DOOR
                        | MWPhysics::Layers::ACTOR);
            }
            mKunaiPos = l.from;
            mKunaiEnd = hit.mHit ? hit.mHitPos : l.to;
            mKunaiDir = l.to - l.from;
            mKunaiDir.normalize();
            mKunaiSticks = hit.mHit && (hit.mHitObject.isEmpty() || !hit.mHitObject.getClass().isActor());
            mKunai = Kunai::Flying;
            if (++mKunaiThrows <= 5)
                Log(Debug::Info) << "Kurenai: thrown kunai " << mKunaiThrows << " from " << l.from.x() << ","
                                 << l.from.y() << "," << l.from.z() << ", "
                                 << (mKunaiSticks      ? "sticks "
                                            : hit.mHit ? "hits an actor "
                                                       : "lands nowhere ")
                                 << (mKunaiEnd - l.from).length() << " away";
        }
        if (mKunai == Kunai::Hidden)
            return;
        if (!mKunaiNode && !makeKunai())
            return;
        if (mKunai == Kunai::Flying)
        {
            // Needs measuring against the game; close to its kunai's look in flight.
            const float step = kKunaiSpeed * kGameHz * t.scale * dt;
            const osg::Vec3f left = mKunaiEnd - mKunaiPos;
            if (left.length() <= step)
            {
                mKunaiPos = mKunaiEnd;
                mKunai = mKunaiSticks ? Kunai::Stuck : Kunai::Hidden;
                mKunaiTimer = 4.0f;
            }
            else
                mKunaiPos += mKunaiDir * step;
        }
        else if ((mKunaiTimer -= dt) <= 0)
            mKunai = Kunai::Hidden;
        if (mKunai == Kunai::Hidden)
        {
            mKunaiNode->setNodeMask(0u);
            return;
        }
        mKunaiNode->setMatrix(mKunaiMesh * osg::Matrixf::scale(osg::Vec3f(1, 1, 1) * t.scale)
            * osg::Matrixf::rotate(osg::Vec3f(1, 0, 0), mKunaiDir) * osg::Matrixf::translate(mKunaiPos));
        mKunaiNode->setNodeMask(MWRender::Mask_Effect);
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
        // Moved on (or blended) the same way as her, so she and the camera keep together between the game's states;
        // a camera cut (a step of more than a few metres) is taken as it is.
        kurenai::proto::Vec3 eyeRn = mLatest->cameraEye, targetRn = mLatest->cameraTarget;
        if (mPrev && (mPrev->flags & kurenai::proto::FlagCamera) && mBlend != 1.0f)
        {
            const kurenai::proto::Vec3& e0 = mPrev->cameraEye;
            const float dx = eyeRn.x - e0.x, dy = eyeRn.y - e0.y, dz = eyeRn.z - e0.z;
            if (dx * dx + dy * dy + dz * dz < 300.0f * 300.0f)
            {
                eyeRn = lerp(e0, eyeRn, mBlend);
                targetRn = lerp(mPrev->cameraTarget, targetRn, mBlend);
            }
        }
        const kurenai::proto::Vec3 eye = t.position(eyeRn);
        const kurenai::proto::Vec3 target = t.position(targetRn);
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
        camera->setPitch(std::clamp(std::asin(std::clamp(dir.z(), -1.0f, 1.0f)) - mSwimPitch, -1.5f, 1.5f), true);
        if (mGameLens)
            applyGameLens();
    }

    void Driver::applyGameLens()
    {
        // The game's field of view spans a 4:3 screen across; Morrowind's is vertical (the window's width follows).
        const float across = mLatest->cameraFov;
        if (!(across > 0.05f && across < 3.0f))
            return;
        MWRender::RenderingManager* rendering = MWBase::Environment::get().getWorld()->getRenderingManager();
        if (!mLensTaken)
        {
            mSavedFov = Settings::camera().mFieldOfView;
            mSavedViewDistance = rendering->getViewDistance();
            mAppliedFov = mAppliedViewDistance = 0;
            mLensTaken = true;
        }
        const float vertical = 2.0f * std::atan(std::tan(across / 2.0f) * 3.0f / 4.0f) * 180.0f / static_cast<float>(M_PI);
        if (std::abs(vertical - mAppliedFov) > 0.05f)
        {
            if (mAppliedFov == 0)
                Log(Debug::Info) << "Kurenai: Red Ninja's lens: " << vertical << " degrees high (Morrowind's was "
                                 << mSavedFov << ")";
            rendering->setFieldOfView(vertical);
            mAppliedFov = vertical;
        }
        if (mLatest->cameraFar > 0)
        {
            const float cap = mMaxViewDistance > 0 ? mMaxViewDistance : mSavedViewDistance;
            const float far = std::min(mLatest->cameraFar * mTransform.scale, cap);
            // Changing it reloads terrain: only for a real change.
            if (std::abs(far - mAppliedViewDistance) > 0.02f * std::max(far, 1.0f))
            {
                rendering->setViewDistance(far, true);
                mAppliedViewDistance = far;
            }
        }
    }

    void Driver::releaseCamera()
    {
        if (mLensTaken)
        {
            MWRender::RenderingManager* rendering = MWBase::Environment::get().getWorld()->getRenderingManager();
            rendering->setFieldOfView(mSavedFov);
            if (mAppliedViewDistance > 0)
                rendering->setViewDistance(mSavedViewDistance, true);
            mLensTaken = false;
        }
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
        if (!mSmokeFlight.empty())
        {
            // The thrown bomb along its arc; its cloud starts where it lands.
            mSmokeFlightTime += dt;
            const float at = mSmokeFlightTime / kSmokeStep;
            const std::size_t i = static_cast<std::size_t>(at);
            if (i + 1 >= mSmokeFlight.size())
            {
                startSmoke(mSmokeFlight.back(), true);
                mSmokeFlight.clear();
                if (mSmokeBombNode)
                    mSmokeBombNode->setNodeMask(0u);
            }
            else if (mSmokeBombNode)
            {
                const float f = at - static_cast<float>(i);
                mSmokeBombNode->setMatrix(osg::Matrixf::translate(mSmokeFlight[i] * (1 - f) + mSmokeFlight[i + 1] * f));
                mSmokeBombNode->setNodeMask(MWRender::Mask_Effect);
            }
        }
        if (!mLink || !mControlling || !mLatest)
            return;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWWorld::Ptr player = world->getPlayerPtr();
        if (!mAnchored || mReanchor)
        {
            mReanchor = false;
            // Kurenai starts where the player stands; from then on her motion is the game's. Under water she starts at
            // the surface: the game swims only there (its water test looks for the surface just above her).
            osg::Vec3f p = player.getRefData().getPosition().asVec3();
            if (mReanchorAtSafe)
            {
                // Back on the last solid ground she stood on: her place in Red Ninja's world maps to it from now on.
                mReanchorAtSafe = false;
                p = mSafe;
                mPrev.reset();
            }
            const MWWorld::CellStore* cell = player.isInCell() ? player.getCell() : nullptr;
            if (cell && cell->getCell()->hasWater() && p.z() < cell->getWaterLevel())
                p.z() = cell->getWaterLevel();
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
        // Interpolate: between her last two states (up to one game frame behind). Predict: past the newest state at
        // her last step's speed, for at most one game frame (no delay; a jump of more than a few metres, as a warp or a
        // new anchor, is not carried on).
        const kurenai::proto::PlayerState& a = mPrev ? *mPrev : *mLatest;
        float blend = (mPrev && mInterpolate) ? std::fmin(1.0f, mSinceLatest * kGameHz) : 1.0f;
        if (mPrev && mPredict)
        {
            const float dx = mLatest->position.x - a.position.x, dy = mLatest->position.y - a.position.y,
                        dz = mLatest->position.z - a.position.z;
            blend
                = dx * dx + dy * dy + dz * dz < 200.0f * 200.0f ? 1.0f + std::fmin(1.0f, mSinceLatest * kGameHz) : 1.0f;
        }
        mBlend = blend;
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
        if (mRecoverNow)
        {
            mRecoverNow = false;
            mVoidTime = 0.f;
            if (mHaveSafe)
            {
                mReanchorAtSafe = true;
                mReanchor = true;
                MWBase::Environment::get().getWindowManager()->messageBox("Kurenai: back on solid ground");
                Log(Debug::Info) << "Kurenai: recover to (" << mSafe.x() << ", " << mSafe.y() << ", " << mSafe.z() << ")";
                return;
            }
            MWBase::Environment::get().getWindowManager()->messageBox("Kurenai: no solid ground to go back to yet");
        }
        updateSwim(dt, mw);
        {
            // Remember the last solid ground she stood on, and watch for the void: nothing at all under her while the
            // game has her in the air (her game collision is answered from Morrowind, so she then falls for good).
            const bool airborne = (mLatest->flags & kurenai::proto::FlagAirborne) != 0;
            const MWPhysics::RayCastingResult under = world->getRayCasting()->castRay(
                osg::Vec3f(mw.x, mw.y, mw.z + 300.0f), osg::Vec3f(mw.x, mw.y, mw.z - 4000.0f), { player }, {});
            if (under.mHit && !airborne && !mSwimming.load() && std::fabs(mw.z - under.mHitPos.z()) < 150.0f)
            {
                mSafeTimer += dt;
                if (mSafeTimer >= 0.5f)
                {
                    mSafe = osg::Vec3f(mw.x, mw.y, under.mHitPos.z());
                    mHaveSafe = true;
                    mSafeTimer = 0.f;
                }
            }
            else if (airborne)
                mSafeTimer = 0.f;
            mVoidTime = (!under.mHit && airborne) ? mVoidTime + dt : 0.f;
            if (mHaveSafe && (mVoidTime > 1.0f || mw.z < mSafe.z() - 8000.0f))
            {
                mRecoverNow = true;
                Log(Debug::Info) << "Kurenai: nothing under her (void) at (" << mw.x << ", " << mw.y << ", " << mw.z
                                 << "): back to the last solid ground";
            }
        }
        const float yaw = lerpAngle(t.yaw(a.rotation, { 0, 0, 1 }), t.yaw(mLatest->rotation, { 0, 0, 1 }), blend);

        {
            // What goes back to the game (proxies, hook places) is measured from where the game has her, so showing
            // her ahead of its newest state never changes what the game does.
            const kurenai::proto::Vec3 at = t.position(mLatest->position);
            mGameFeet = osg::Vec3f(at.x, at.y, mw.z);
        }
        world->moveObject(player, osg::Vec3f(mw.x, mw.y, mw.z), true, true);
        player = world->getPlayerPtr();
        mLookPitch = player.getRefData().getPosition().rot[0] - mSwimPitch; // put back to mSwimPitch every frame
        // While she swims, the mouse looks up and down as in Morrowind (the dive's direction, and the camera's tilt).
        mSwimPitch = mSwimming.load() ? std::clamp(player.getRefData().getPosition().rot[0], -1.4f, 1.4f) : 0.0f;
        world->rotateObject(player, osg::Vec3f(mSwimPitch, 0, yaw), MWBase::RotationFlag_none);
        if (mProxyEnabled)
            sendProxyTargets(t);
        if (mHooks)
            sendHookTargets(t, dt);
        if (mLockOn)
        {
            updateLockOn(t);
            updateLockMarker();
        }
        applyAnimation(mLatest->animId);
        updateSneak(mLatest->animId);
        updateNinjutsu(mLatest->animId, dt);
        updateKiton(mLatest->animId, mLatest->kiton);
        updateHealth(mLatest->health, mLatest->healthMax);
        applyHeals(mLatest->flags);
        if (mLatest->animId != mItemMotion)
        {
            mItemMotion = mLatest->animId;
            if (smokeMotions().count(mItemMotion))
            {
                // Thrown along the arc she aims (or would aim: a throw the game started by itself goes the same way).
                smokeArc(player, mSmokeFlight);
                mSmokeFlightTime = 0;
                const osg::Vec3f& land = mSmokeFlight.back();
                Log(Debug::Info) << "Kurenai: smoke bomb thrown, elevation " << osg::RadiansToDegrees(mSmokeElev)
                                 << " degrees, lands at (" << land.x() << ", " << land.y() << ", " << land.z() << ") in "
                                 << (mSmokeFlight.size() - 1) * kSmokeStep << " s";
            }
        }
        updateSmokeAim(player, dt);
        updatePoseOverride();
        if (mNative)
        {
            const bool shown = mNative->update(player, mPs2Anim == Ps2Anim::All,
                mLatestPose ? &*mLatestPose : nullptr, &*mLatest, t.scale, mKitonOn);
            if (shown != mNativeShown)
                Log(Debug::Info) << "Kurenai: " << (shown ? "her own PS2 body" : "Morrowind's body") << " shows";
            mNativeShown = shown;
        }
        updateWire(t);
        updateKunai(t, dt);
        if (mGameCameraMode)
            applyGameCamera();
        if (mLog)
            std::fprintf(mLog, "%llu\t%llu\t\t\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.4f\t%u\n",
                static_cast<unsigned long long>(mFrames), static_cast<unsigned long long>(kurenai::monotonicNs()),
                rn.x, rn.y, rn.z, mw.x, mw.y, mw.z, yaw, mLatest->animId);
    }
}
