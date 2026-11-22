#include "luamanagerimp.hpp"
#include "waistcontact.hpp"
#include "limbcontact.hpp"

#include <filesystem>
#include <algorithm>
#include <cmath>
#include <optional>

#include <MyGUI_InputManager.h>
#include <osg/Stats>

#include <sol/object.hpp>
#include <sol/table.hpp>
#include <sol/types.hpp>

#include <components/debug/debuglog.hpp>

#include <components/esm/luascripts.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loadweap.hpp>
#include <components/esm3/loadarmo.hpp>

#include <components/settings/values.hpp>

#include <components/l10n/manager.hpp>

#include <components/lua_ui/registerscriptsettings.hpp>
#include <components/lua_ui/util.hpp>

#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwrender/bonegroup.hpp"
#include "../mwrender/animation.hpp"
#include "../mwrender/npcanimation.hpp"
#include "../mwrender/postprocessor.hpp"
#include "../mwmechanics/brutaloptions.hpp"
#include "../mwmechanics/weapontype.hpp"
#include "../mwmechanics/combat.hpp"
#include "../mwworld/class.hpp"
#include "../mwmechanics/creaturestats.hpp"

#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/scene.hpp"
#include "../mwworld/worldmodel.hpp"

#include "luabindings.hpp"
#include "playerscripts.hpp"
#include "types/types.hpp"
#include "userdataserializer.hpp"

#include "../mwvr/vrgui.hpp"
#include <components/vr/vr.hpp>

namespace MWLua
{
    namespace
    {
        struct BoolScopeGuard
        {
            bool& mValue;
            BoolScopeGuard(bool& value)
                : mValue(value)
            {
                mValue = true;
            }

            ~BoolScopeGuard() { mValue = false; }
        };

        LocalScripts* asLocal(const LuaUtil::ScriptsContainerWeakPtr& ptr)
        {
            auto scripts = static_cast<LocalScripts*>(*ptr);
            if (scripts == nullptr)
                Log(Debug::Warning) << "Found local Lua script that outlived its object";
            return scripts;
        }

        struct MeleeHeadContact
        {
            osg::Vec3f origin, direction, head, neck, boundary, axis;
            std::optional<osg::Vec3f> point;
            bool anatomical = false;
        };

        struct MeleeWaistContact
        {
            osg::Vec3f center, axis;
            float halfWidth = 0.f, lower = 0.f, upper = 0.f, radius = 0.f;
            bool anatomical = false, hit = false, aimAssisted = false;
        };

        struct MeleeLimbContact
        {
            bool legHit = false, legsAllowed = false;
            unsigned closestArm = 0;
        };

        MeleeLimbContact classifyMeleeLimbContact(const MWWorld::Ptr& attacker,
            const MWWorld::Ptr& victim, const osg::Vec3f& hitPosition, int contactPart)
        {
            MeleeLimbContact result;
            const auto world = MWBase::Environment::get().getWorld();
            if (attacker != world->getPlayerPtr() || victim.isEmpty() || !victim.getClass().isNpc()
                || victim == attacker) return result;
            const auto* animation = dynamic_cast<const MWRender::NpcAnimation*>(world->getAnimation(victim));
            if (!animation) return result;
            result.legsAllowed = animation->canSeverLegs();
            const auto bone = [animation](const char* name) -> std::optional<osg::Vec3f> {
                if (const auto m = animation->getBoneWorldMatrix(name)) return osg::Vec3f(m->getTrans());
                return {};
            };
            const auto pelvis = bone("Bip01 Pelvis"), neck = bone("Bip01 Neck");
            const auto leftFoot = bone("Bip01 L Foot"), rightFoot = bone("Bip01 R Foot");
            if (pelvis && neck && leftFoot && rightFoot)
                if (const auto region = LimbContact::legs(*pelvis, *neck, *leftFoot, *rightFoot))
                    result.legHit = region->contains(hitPosition, contactPart);
            const auto ls = bone("Bip01 L UpperArm"), le = bone("Bip01 L Forearm"), lh = bone("Bip01 L Hand");
            const auto rs = bone("Bip01 R UpperArm"), re = bone("Bip01 R Forearm"), rh = bone("Bip01 R Hand");
            if (ls && le && lh && rs && re && rh)
                result.closestArm = LimbContact::closestArm(hitPosition, *ls, *le, *lh, *rs, *re, *rh);
            return result;
        }

        MeleeWaistContact classifyMeleeWaistContact(const MWWorld::Ptr& attacker,
            const MWWorld::Ptr& victim, const osg::Vec3f& hitPosition, int contactPart)
        {
            MeleeWaistContact result;
            auto world = MWBase::Environment::get().getWorld();
            if (attacker != world->getPlayerPtr() || victim.isEmpty() || !victim.getClass().isNpc()
                || victim == attacker) return result;
            const auto* animation = world->getAnimation(victim);
            if (!animation) return result;
            const auto pelvis = animation->getBoneWorldMatrix("Bip01 Pelvis");
            const auto waist = animation->getBoneWorldMatrix("Bip01 Spine1");
            const auto neck = animation->getBoneWorldMatrix("Bip01 Neck");
            if (!pelvis || !waist || !neck) return result;
            const auto region = WaistContact::make(pelvis->getTrans(), waist->getTrans(), neck->getTrans());
            if (!region) return result;
            result.center = region->center;
            result.axis = region->axis;
            result.anatomical = true;
            result.halfWidth = region->halfWidth;
            result.lower = region->lower;
            result.upper = region->upper;
            result.radius = region->radius;
            result.hit = region->contains(hitPosition, contactPart);
            if (!result.hit)
            {
                const auto [origin, direction] = MWMechanics::getMeleeAim(attacker);
                result.hit = region->containsAimedContact(hitPosition, contactPart, origin, direction);
                result.aimAssisted = result.hit;
            }
            return result;
        }

        // Classify the same aimed contact that selected the melee victim as a neck/head blow.
        MeleeHeadContact classifyMeleeHeadContact(const MWWorld::Ptr& attacker,
            const MWWorld::Ptr& victim, const osg::Vec3f& hitPosition, int contactPart)
        {
            MeleeHeadContact result;
            auto world = MWBase::Environment::get().getWorld();
            if (attacker != world->getPlayerPtr() || victim.isEmpty() || !victim.getClass().isNpc()
                || victim == attacker)
                return result;
            const auto [origin,direction]=MWMechanics::getMeleeAim(attacker);
            result.origin=origin;
            result.direction=direction;
            const auto* targetAnimation=world->getAnimation(victim);
            if(!targetAnimation)return result;
            const auto head=targetAnimation->getBoneWorldMatrix("Bip01 Head");
            const auto neck=targetAnimation->getBoneWorldMatrix("Bip01 Neck");
            if(!head || !neck)return result;
            result.head=head->getTrans();result.neck=neck->getTrans();
            result.axis=result.head-result.neck;
            const float length=result.axis.normalize();
            if(!(length>.01f) || !std::isfinite(length))return result;
            result.boundary=result.head-result.axis*(length*.25f);
            result.anatomical=true;
            // Any blow into the neck or head counts: a head, hair or neck mesh, a contact at or
            // above the base of the neck (collars and pauldrons), or an aim line passing close
            // to the neck-to-skull segment. A chest-high blow stays below all three.
            constexpr float neckMargin=3.f, aimTolerance=12.f;
            if(contactPart==ESM::PRT_Head || contactPart==ESM::PRT_Hair || contactPart==ESM::PRT_Neck
                || (hitPosition-result.neck)*result.axis>=-neckMargin)
            {
                result.point=hitPosition;
                return result;
            }
            const osg::Vec3f top=result.head+result.axis*(length*.6f);
            const osg::Vec3f segment=top-result.neck;
            const osg::Vec3f w=origin-result.neck;
            const float b=direction*segment, c=segment*segment, d=direction*w, e=segment*w;
            const float denominator=c-b*b; // direction is unit length
            float s=denominator>1e-4f ? std::clamp((e-b*d)/denominator,0.f,1.f) : 0.f;
            float along=std::max(0.f,b*s-d);
            s=std::clamp((along*b+e)/c,0.f,1.f); // re-project after clamping the ray parameter
            along=std::max(0.f,b*s-d);
            const osg::Vec3f onSegment=result.neck+segment*s;
            if((origin+direction*along-onSegment).length()<=aimTolerance)
                result.point=onSegment;
            return result;
        }

    }

    static LuaUtil::LuaStateSettings createLuaStateSettings()
    {
        if (!Settings::lua().mLuaProfiler)
            LuaUtil::LuaState::disableProfiler();
        return { .mInstructionLimit = Settings::lua().mInstructionLimitPerCall,
            .mMemoryLimit = Settings::lua().mMemoryLimit,
            .mSmallAllocMaxSize = Settings::lua().mSmallAllocMaxSize,
            .mLogMemoryUsage = Settings::lua().mLogMemoryUsage };
    }

    LuaManager::LuaManager(const VFS::Manager* vfs, const std::filesystem::path& libsDir)
        : mLua(vfs, &mConfiguration, createLuaStateSettings())
    {
        Log(Debug::Info) << "Lua version: " << LuaUtil::getLuaVersion();
        mLua.addInternalLibSearchPath(libsDir);

        mGlobalSerializer = createUserdataSerializer(false);
        mLocalSerializer = createUserdataSerializer(true);
        mGlobalLoader = createUserdataSerializer(false, &mContentFileMapping);
        mLocalLoader = createUserdataSerializer(true, &mContentFileMapping);

        mGlobalScripts.setSerializer(mGlobalSerializer.get());
    }

    LuaManager::~LuaManager()
    {
        LuaUi::clearSettings();
    }

    void LuaManager::onVRFrame() 
    {
        mMenuScripts.onVRFrame();
        PlayerScripts* playerScripts
            = mPlayer.isEmpty() ? nullptr : dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        if (playerScripts)
            playerScripts->onVRFrame();
    }

    void LuaManager::initConfiguration(bool reload)
    {
        mConfiguration.init(MWBase::Environment::get().getESMStore()->getLuaScriptsCfg(), reload);
        Log(Debug::Verbose) << "Lua scripts configuration (" << mConfiguration.size() << " scripts):";
        for (size_t i = 0; i < mConfiguration.size(); ++i)
            Log(Debug::Verbose) << "#" << i << " " << LuaUtil::scriptCfgToString(mConfiguration[i]);
        mMenuScripts.setAutoStartConf(mConfiguration.getMenuConf());
        mGlobalScripts.setAutoStartConf(mConfiguration.getGlobalConf());
    }

    void LuaManager::initPreLoad()
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            Context context;
            context.mType = Context::Load;
            context.mLuaManager = this;
            context.mLua = &mLua;

            for (const auto& [name, package] : initCommonPackages(context))
                mLua.addCommonPackage(name, package);

            for (const auto& [name, package] : initLoadPackages(context))
                mLoadScripts.addPackage(name, package);

            mLoadScripts.addPackage("openmw.storage", LuaUtil::LuaStorage::initLoadPackage(view, &mPlayerStorage));

            LuaUtil::LuaStorage::initLuaBindings(view);
        });
    }

    void LuaManager::contentFilesLoaded()
    {
        initConfiguration(false);
        mLoadScripts.setAutoStartConf(mConfiguration.getLoadConf());
        mLoadScripts.addAutoStartedScripts();
        mLoadScripts.contentFilesLoaded();
        mLoadScripts.removeAllScripts();
    }

    void LuaManager::initPostLoad()
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            Context globalContext;
            globalContext.mType = Context::Global;
            globalContext.mLuaManager = this;
            globalContext.mLua = &mLua;
            globalContext.mObjectLists = &mObjectLists;
            globalContext.mLuaEvents = &mLuaEvents;
            globalContext.mSerializer = mGlobalSerializer.get();

            Context localContext = globalContext;
            localContext.mType = Context::Local;
            localContext.mSerializer = mLocalSerializer.get();

            Context menuContext = globalContext;
            menuContext.mType = Context::Menu;

            for (const auto& [name, package] : initGlobalPackages(globalContext))
                mGlobalScripts.addPackage(name, package);
            for (const auto& [name, package] : initMenuPackages(menuContext))
                mMenuScripts.addPackage(name, package);

            mLocalPackages = initLocalPackages(localContext);

            mPlayerPackages = initPlayerPackages(localContext);
            mPlayerPackages.insert(mLocalPackages.begin(), mLocalPackages.end());

            mGlobalScripts.addPackage("openmw.storage", LuaUtil::LuaStorage::initGlobalPackage(view, &mGlobalStorage));
            mMenuScripts.addPackage(
                "openmw.storage", LuaUtil::LuaStorage::initMenuPackage(view, &mGlobalStorage, &mPlayerStorage));
            mLocalPackages["openmw.storage"] = LuaUtil::LuaStorage::initLocalPackage(view, &mGlobalStorage);
            mPlayerPackages["openmw.storage"]
                = LuaUtil::LuaStorage::initPlayerPackage(view, &mGlobalStorage, &mPlayerStorage);

            mPlayerStorage.setActive(true);
            mGlobalStorage.setActive(false);

            mInitialized = true;
            mMenuScripts.addAutoStartedScripts();
        });
    }

    void LuaManager::loadPermanentStorage(const std::filesystem::path& userConfigPath)
    {
        mPlayerStorage.setActive(true);
        mGlobalStorage.setActive(true);
        const auto globalPath = userConfigPath / "global_storage.bin";
        const auto playerPath = userConfigPath / "player_storage.bin";

        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            if (std::filesystem::exists(globalPath))
                mGlobalStorage.load(view.sol(), globalPath);
            if (std::filesystem::exists(playerPath))
                mPlayerStorage.load(view.sol(), playerPath);
        });
    }

    void LuaManager::savePermanentStorage(const std::filesystem::path& userConfigPath)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            if (mGlobalScriptsStarted)
                mGlobalStorage.save(view.sol(), userConfigPath / "global_storage.bin");
            mPlayerStorage.save(view.sol(), userConfigPath / "player_storage.bin");
        });
    }

    void LuaManager::sendLocalEvent(
        const MWWorld::Ptr& target, const std::string& name, const std::optional<sol::table>& data)
    {
        LuaUtil::BinaryData binary = {};
        if (data)
        {
            binary = LuaUtil::serialize(*data, mLocalSerializer.get());
        }
        mLuaEvents.addLocalEvent({ getId(target), name, std::move(binary) });
    }

    void LuaManager::update()
    {
        if (const int steps = Settings::lua().mGcStepsPerFrame; steps > 0)
            lua_gc(mLua.unsafeState(), LUA_GCSTEP, steps);

        if (mPlayer.isEmpty())
            return; // The game is not started yet.

        MWWorld::Ptr newPlayerPtr = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (!(getId(mPlayer) == getId(newPlayerPtr)))
            throw std::logic_error("Player RefNum was changed unexpectedly");
        if (!mPlayer.isInCell() || !newPlayerPtr.isInCell() || mPlayer.getCell() != newPlayerPtr.getCell())
        {
            mPlayer = newPlayerPtr; // player was moved to another cell, update ptr in registry
            MWBase::Environment::get().getWorldModel()->registerPtr(mPlayer);
        }

        // Every stage below runs under its own guard (and the event queues guard each event, see
        // LuaEvents / EngineEvents), so one failing script, event or callback is logged with where it failed and
        // skipped, instead of aborting the whole frame. Aborting the frame used to drop every queued engine event
        // too - activations included - which left the player unable to activate anything until the failure
        // stopped. guardedStage() rethrows nothing.
        guardedStage("object lists", [&] { mObjectLists.update(); });

        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mQueuedAutoStartedScripts)
        {
            guardedStage("auto-started scripts", [&] {
                if (LocalScripts* scripts = asLocal(ptr))
                    scripts->addAutoStartedScripts();
            });
        }
        mQueuedAutoStartedScripts.clear();

        guardedStage("active scripts list", [&] {
            std::erase_if(mActiveLocalScripts, [](const LuaUtil::ScriptsContainerWeakPtr& ptr) {
                LocalScripts* l = asLocal(ptr);
                return l == nullptr || l->getPtrOrEmpty().isEmpty() || l->getPtrOrEmpty().mRef->isDeleted();
            });
        });

        guardedStage("stats", [&] {
            mGlobalScripts.statsNextFrame();
            for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
                asLocal(ptr)->statsNextFrame();
        });

        mLuaEvents.finalizeEventBatch();

        MWWorld::DateTimeManager& timeManager = *MWBase::Environment::get().getWorld()->getTimeManager();
        if (!timeManager.isPaused())
        {
            guardedStage("menu timers",
                [&] { mMenuScripts.processTimers(timeManager.getSimulationTime(), timeManager.getGameTime()); });
            guardedStage("global timers",
                [&] { mGlobalScripts.processTimers(timeManager.getSimulationTime(), timeManager.getGameTime()); });
            for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
            {
                guardedStage("local timers", [&] {
                    if (LocalScripts* l = asLocal(ptr))
                        l->processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
                });
            }
        }

        // Run event handlers for events that were sent before `finalizeEventBatch` (each event is guarded).
        mLuaEvents.callEventHandlers(
            [this](std::string_view what, const std::exception* e) { reportFailure(what, e); });

        // Run queued callbacks
        std::vector<CallbackWithData> callbacks;
        callbacks.swap(mQueuedCallbacks);
        for (CallbackWithData& c : callbacks)
            guardedStage("queued callback",
                [&] { mLua.protectedCall([&](LuaUtil::LuaView&) { c.mCallback.tryCall(c.mArg); }); });

        // Run engine handlers (each event is guarded)
        guardedStage("engine events", [&] {
            mLua.protectedCall([&](LuaUtil::LuaView&) {
                mEngineEvents.callEngineHandlers(
                    [this](std::string_view what, const std::exception* e) { reportFailure(what, e); });
            });
        });

        const bool isPaused = timeManager.isPaused();
        const float frameDuration = MWBase::Environment::get().getFrameDuration();
        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
        {
            guardedStage("local onUpdate", [&] {
                mLua.protectedCall([&](LuaUtil::LuaView&) {
                    if (LocalScripts* l = asLocal(ptr))
                        l->update(isPaused ? 0 : frameDuration);
                });
            });
        }
        guardedStage("global onUpdate", [&] {
            mLua.protectedCall([&](LuaUtil::LuaView&) { mGlobalScripts.update(isPaused ? 0 : frameDuration); });
        });
        guardedStage("script unloading",
            [&] { mLua.protectedCall([&](LuaUtil::LuaView& lua) { mScriptTracker.unloadInactiveScripts(lua); }); });
    }

    void LuaManager::reportFailure(std::string_view what, const std::exception* e)
    {
        // Throttled per place: the first 3 failures of each are logged, then one in 600 (a failure that repeats
        // every frame would otherwise flood the log).
        int& count = mFailureCounts[std::string(what)];
        ++count;
        if (count > 3 && count % 600 != 0)
            return;
        std::string message = e ? e->what() : "";
        if (message.empty())
            message = e ? std::string("(empty message, exception type ") + typeid(*e).name() + ")"
                        : "(not a std::exception)";
        Log(Debug::Error) << "Lua: " << what << " failed: " << message
                          << (count > 3 ? " (repeated " + std::to_string(count) + " times)" : "");
    }

    void LuaManager::objectTeleported(const MWWorld::Ptr& ptr)
    {
        if (ptr == mPlayer)
        {
            // For player run the onTeleported handler immediately,
            // so it can adjust camera position after teleporting.
            PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
            if (playerScripts)
                playerScripts->onTeleported();
        }
        else
            mEngineEvents.addToQueue(EngineEvents::OnTeleported{ getId(ptr) });
    }

    void LuaManager::questUpdated(const ESM::RefId& questId, int stage)
    {
        if (mPlayer.isEmpty())
            return; // The game is not started yet.
        PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        if (playerScripts)
        {
            playerScripts->onQuestUpdate(questId.serializeText(), stage);
        }
    }

    void LuaManager::synchronizedUpdate()
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) { synchronizedUpdateUnsafe(); });
    }

    void LuaManager::synchronizedUpdateUnsafe()
    {
        if (mNewGameStarted)
        {
            mNewGameStarted = false;
            // Run onNewGame handler in synchronizedUpdate (at the beginning of the frame), so it
            // can teleport the player to the starting location before the first frame is rendered.
            mGlobalScripts.newGameStarted();
        }
        BoolScopeGuard updateGuard(mRunningSynchronizedUpdates);

        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        PlayerScripts* playerScripts
            = mPlayer.isEmpty() ? nullptr : dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        // We apply input events in `synchronizedUpdate` rather than in `update` in order to reduce input latency.
        {
            BoolScopeGuard processingGuard(mProcessingInputEvents);

            for (const auto& event : mMenuInputEvents)
                mMenuScripts.processInputEvent(event);
            mMenuInputEvents.clear();
            if (playerScripts && !windowManager->containsMode(MWGui::GM_MainMenu))
            {
                for (const auto& event : mInputEvents)
                    playerScripts->processInputEvent(event);
            }
            mInputEvents.clear();
            mLuaEvents.callMenuEventHandlers();
            float frameDuration = MWBase::Environment::get().getWorld()->getTimeManager()->isPaused()
                ? 0.f
                : MWBase::Environment::get().getFrameDuration();
            mInputActions.update(frameDuration);
            mMenuScripts.onFrame(frameDuration);
            if (playerScripts)
                playerScripts->onFrame(frameDuration);
        }

        for (const auto& [message, mode] : mUIMessages)
            windowManager->messageBox(message, mode);
        mUIMessages.clear();
        for (auto& [msg, color] : mInGameConsoleMessages)
            windowManager->printToConsole(msg, "#" + color.toHex());
        mInGameConsoleMessages.clear();

        applyDelayedActions();

        if (mReloadAllScriptsRequested)
        {
            // Reloading right after `applyDelayedActions` to guarantee that no delayed actions are currently queued.
            reloadAllScriptsImpl();
            mReloadAllScriptsRequested = false;
        }

        if (mDelayedUiModeChangedArg)
        {
            if (playerScripts)
                playerScripts->uiModeChanged(*mDelayedUiModeChangedArg, true);
            mDelayedUiModeChangedArg = std::nullopt;
        }
    }

    void LuaManager::applyDelayedActions()
    {
        BoolScopeGuard applyingGuard(mApplyingDelayedActions);
        for (DelayedAction& action : mActionQueue)
            action.apply();
        mActionQueue.clear();

        if (mTeleportPlayerAction)
            mTeleportPlayerAction->apply();
        mTeleportPlayerAction.reset();
    }

    void LuaManager::clear()
    {
        if (VR::getVR())
            MWVR::VRGUIManager::instance().clearLua();
        LuaUi::clearGameInterface();
        mUiResourceManager.clear();
        MWBase::Environment::get().getWorld()->getPostProcessor()->disableDynamicShaders();
        mActiveLocalScripts.clear();
        mLuaEvents.clear();
        mEngineEvents.clear();
        mInputEvents.clear();
        mMenuInputEvents.clear();
        mObjectLists.clear();
        mGlobalScripts.removeAllScripts();
        mGlobalScriptsStarted = false;
        mNewGameStarted = false;
        mDelayedUiModeChangedArg = std::nullopt;
        if (!mPlayer.isEmpty())
        {
            mPlayer.getCellRef().unsetRefNum();
            mPlayer.getRefData().setLuaScripts(nullptr);
            mPlayer = MWWorld::Ptr();
        }
        mGlobalStorage.setActive(true);
        mGlobalStorage.clearTemporaryAndRemoveCallbacks();
        mGlobalStorage.setActive(false);
        mPlayerStorage.clearTemporaryAndRemoveCallbacks();
        mInputActions.clear();
        mInputTriggers.clear();
        mQueuedAutoStartedScripts.clear();
        for (int i = 0; i < 5; ++i)
            lua_gc(mLua.unsafeState(), LUA_GCCOLLECT, 0);
    }

    void LuaManager::setupPlayer(const MWWorld::Ptr& ptr)
    {
        if (!mInitialized)
            return;
        if (!mPlayer.isEmpty())
            throw std::logic_error("Player is initialized twice");
        mObjectLists.objectAddedToScene(ptr);
        mObjectLists.setPlayer(ptr);
        mPlayer = ptr;
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
        {
            localScripts = createLocalScripts(ptr);
            mQueuedAutoStartedScripts.push_back(localScripts->getWeakPointer());
        }
        mActiveLocalScripts.insert(localScripts->getWeakPointer());
        mEngineEvents.addToQueue(EngineEvents::OnActive{ getId(ptr) });
    }

    void LuaManager::newGameStarted()
    {
        mGlobalStorage.setActive(true);
        mInputEvents.clear();
        mGlobalScripts.addAutoStartedScripts();
        mGlobalScriptsStarted = true;
        mNewGameStarted = true;
    }

    void LuaManager::gameLoaded()
    {
        mGlobalStorage.setActive(true);
        if (!mGlobalScriptsStarted)
            mGlobalScripts.addAutoStartedScripts();
        mGlobalScriptsStarted = true;
        mMenuScripts.stateChanged();
    }

    void LuaManager::gameEnded()
    {
        // TODO: disable scripts and global storage when the game is actually unloaded
        // mGlobalStorage.setActive(false);
        mMenuScripts.stateChanged();
    }

    void LuaManager::noGame()
    {
        clear();
        mMenuScripts.stateChanged();
    }

    void LuaManager::uiModeChanged(const MWWorld::Ptr& arg)
    {
        if (mPlayer.isEmpty())
            return;
        ObjectId argId = arg.isEmpty() ? ObjectId() : getId(arg);
        if (mApplyingDelayedActions)
        {
            mDelayedUiModeChangedArg = argId;
            return;
        }
        PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        if (playerScripts)
            playerScripts->uiModeChanged(argId, false);
    }

    void LuaManager::vrRecentered(bool vertical, bool horizontal)
    {
        mMenuScripts.onVRRecenter(vertical, horizontal);
        if (mPlayer.isEmpty())
            return;
        PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        if (playerScripts)
            playerScripts->onVRRecenter(vertical, horizontal);
    }

    void LuaManager::actorDied(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty())
            return;
        mLuaEvents.addLocalEvent({ getId(actor), "Died", {} });
    }

    void LuaManager::onDialogueResponse(
        const MWWorld::Ptr& actor, const ESM::DialInfo& info, const ESM::Dialogue& record)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table data = view.newTable();
            data["actor"] = LObject(actor);
            if (record.mType == ESM::Dialogue::Type::Greeting)
                data["type"] = "greeting";
            else if (record.mType == ESM::Dialogue::Type::Journal)
                data["type"] = "journal";
            else if (record.mType == ESM::Dialogue::Type::Persuasion)
                data["type"] = "persuasion";
            else if (record.mType == ESM::Dialogue::Type::Topic)
                data["type"] = "topic";
            else if (record.mType == ESM::Dialogue::Type::Voice)
                data["type"] = "voice";
            data["infoId"] = info.mId.serializeText();
            data["recordId"] = record.mId.serializeText();
            sendLocalEvent(mPlayer, "DialogueResponse", data);
        });
    }

    void LuaManager::useItem(const MWWorld::Ptr& object, const MWWorld::Ptr& actor, bool force)
    {
        MWBase::Environment::get().getWorldModel()->registerPtr(object);
        mEngineEvents.addToQueue(EngineEvents::OnUseItem{ getId(actor), getId(object), force });
    }

    void LuaManager::animationTextKey(const MWWorld::Ptr& actor, const std::string& key)
    {
        auto pos = key.find(": ");
        if (pos != std::string::npos)
            mEngineEvents.addToQueue(
                EngineEvents::OnAnimationTextKey{ getId(actor), key.substr(0, pos), key.substr(pos + 2) });
    }

    void LuaManager::playAnimation(const MWWorld::Ptr& actor, const std::string& groupname,
        const MWRender::AnimPriority& priority, int blendMask, bool autodisable, float speedmult,
        std::string_view start, std::string_view stop, float startpoint, uint32_t loops, bool loopfallback)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table options = view.newTable();
            options["blendMask"] = blendMask;
            options["autoDisable"] = autodisable;
            options["speed"] = speedmult;
            options["startKey"] = start;
            options["stopKey"] = stop;
            options["startPoint"] = startpoint;
            options["loops"] = loops;
            options["forceLoop"] = loopfallback;

            bool priorityAsTable = false;
            for (uint32_t i = 1; i < MWRender::sNumBlendMasks; i++)
                if (priority[static_cast<MWRender::BoneGroup>(i)] != priority[static_cast<MWRender::BoneGroup>(0)])
                    priorityAsTable = true;
            if (priorityAsTable)
            {
                sol::table priorityTable = view.newTable();
                for (uint32_t i = 0; i < MWRender::sNumBlendMasks; i++)
                    priorityTable[static_cast<MWRender::BoneGroup>(i)] = priority[static_cast<MWRender::BoneGroup>(i)];
                options["priority"] = priorityTable;
            }
            else
                options["priority"] = priority[MWRender::BoneGroup_LowerBody];

            // mEngineEvents.addToQueue(event);
            //  Has to be called immediately, otherwise engine details that depend on animations playing immediately
            //  break.
            if (auto* scripts = actor.getRefData().getLuaScripts())
                scripts->onPlayAnimation(groupname, options);
        });
    }

    void LuaManager::animationEnded(const MWWorld::Ptr& actor, std::string_view groupname, float time, float completion,
        std::string_view startKey, std::string_view stopKey)
    {
        mEngineEvents.addToQueue(EngineEvents::OnAnimationEnded{
            getId(actor), std::string(groupname), std::string(startKey), std::string(stopKey), time, completion });
    }

    void LuaManager::skillUse(const MWWorld::Ptr& actor, ESM::RefId skillId, int useType, float scale)
    {
        mEngineEvents.addToQueue(EngineEvents::OnSkillUse{ getId(actor), skillId.serializeText(), useType, scale });
    }

    void LuaManager::skillLevelUp(const MWWorld::Ptr& actor, ESM::RefId skillId, std::string_view source)
    {
        mEngineEvents.addToQueue(
            EngineEvents::OnSkillLevelUp{ getId(actor), skillId.serializeText(), std::string(source) });
    }

    void LuaManager::jailTimeServed(const MWWorld::Ptr& actor, int days)
    {
        mEngineEvents.addToQueue(EngineEvents::OnJailTimeServed{ getId(actor), days });
    }

    void LuaManager::onHit(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim, const MWWorld::Ptr& weapon,
        const MWWorld::Ptr& ammo, int attackType, float attackStrength, float damage, bool isHealth,
        const osg::Vec3f& hitPos, bool successful, MWMechanics::DamageSourceType sourceType,
        uint64_t brutalAttackId, bool brutalPowered, bool brutalBlocked, int contactPart)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table damageTable = view.newTable();
            if (isHealth)
                damageTable["health"] = damage;
            else
                damageTable["fatigue"] = damage;

            sol::table data = view.newTable();
            if (!attacker.isEmpty())
                data["attacker"] = LObject(attacker);
            if (!weapon.isEmpty())
                data["weapon"] = LObject(weapon);
            if (!ammo.isEmpty())
                data["ammo"] = ammo.getCellRef().getRefId().serializeText();
            data["type"] = attackType;
            data["strength"] = attackStrength;
            data["damage"] = damageTable;
            data["hitPos"] = hitPos;
            data["successful"] = successful;
            data["brutalAttackId"] = brutalAttackId;
            data["brutalPowered"] = brutalPowered;
            data["brutalFast"] = !attacker.isEmpty() && attacker.getClass().isActor()
                && attacker.getClass().getCreatureStats(attacker).mBrutalFastStrike;
            data["brutalShieldRush"] = !attacker.isEmpty() && attacker.getClass().isActor()
                && attacker.getClass().getCreatureStats(attacker).mBrutalShieldRush;
            data["brutalSlashRight"] = !attacker.isEmpty() && attacker.getClass().isActor()
                && attacker.getClass().getCreatureStats(attacker).mBrutalSlashRight;
            data["brutalSlideAttack"] = sourceType == MWMechanics::DamageSourceType::Melee
                && attackType == ESM::Weapon::AT_Slash && !attacker.isEmpty()
                && attacker == MWMechanics::getPlayer()
                && attacker.getClass().getCreatureStats(attacker).mBrutalSlideAttack;
            data["brutalBlocked"] = brutalBlocked;
            data["brutalAttackType"] = sourceType != MWMechanics::DamageSourceType::Melee ? ""
                : attackType == ESM::Weapon::AT_Slash ? "slash"
                : attackType == ESM::Weapon::AT_Chop ? "chop"
                : attackType == ESM::Weapon::AT_Thrust ? "thrust" : "";
            // Anatomical classifiers inspect current geometry. Their metadata
            // is consumed by dismemberment; disabled gore does none of this work.
            const bool classifyAnatomy = sourceType == MWMechanics::DamageSourceType::Melee
                && Settings::game().mBrutalEnabled && Settings::game().mBrutalDismemberment
                && Settings::game().mBrutalRagdoll;
            MeleeHeadContact headContact;
            if (classifyAnatomy)
                headContact = classifyMeleeHeadContact(attacker, victim, hitPos, contactPart);
            data["brutalContactPart"] = contactPart;
            // Attack receipts also exist for original bounds-based targeting;
            // those random blood points are not rendered surface contacts.
            data["brutalContactExact"] = sourceType == MWMechanics::DamageSourceType::Melee
                && brutalAttackId != 0 && Settings::game().mBrutalMeleeGeometry;
            MeleeWaistContact waistContact;
            if (classifyAnatomy)
                waistContact = classifyMeleeWaistContact(attacker, victim, hitPos, contactPart);
            MeleeLimbContact limbContact;
            if (classifyAnatomy)
                limbContact = classifyMeleeLimbContact(attacker, victim, hitPos, contactPart);
            // All legs count, not just the knee plane. Keep lower hits from the
            // generous waist/head-aim fallbacks, including covered skirt legs.
            if (limbContact.legHit)
            {
                waistContact.hit = false;
                headContact.point.reset();
            }
            data["brutalLegHit"] = limbContact.legHit;
            data["brutalLegsAllowed"] = limbContact.legsAllowed;
            data["brutalClosestArm"] = limbContact.closestArm;
            // A near-waist stroke outranks the generous head AIM-line
            // fallback. Real head/hair/neck contacts are excluded above.
            if (waistContact.hit) headContact.point.reset();
            data["brutalHeadHit"] = headContact.point.has_value();
            data["brutalWaistHit"] = waistContact.hit;
            data["brutalWaistAimAssisted"] = waistContact.aimAssisted;
            if (waistContact.anatomical)
            {
                data["brutalWaistPosition"] = waistContact.center;
                data["brutalWaistAxis"] = waistContact.axis;
                data["brutalWaistHalfWidth"] = waistContact.halfWidth;
                data["brutalWaistLowerOffset"] = waistContact.lower;
                data["brutalWaistUpperOffset"] = waistContact.upper;
                data["brutalWaistRadius"] = waistContact.radius;
            }
            if (headContact.anatomical)
            {
                data["brutalAimOrigin"] = headContact.origin;
                data["brutalAimDirection"] = headContact.direction;
                data["brutalHeadPosition"] = headContact.head;
                data["brutalNeckPosition"] = headContact.neck;
                data["brutalHeadBoundary"] = headContact.boundary;
                data["brutalHeadAxis"] = headContact.axis;
            }
            if (headContact.point)
                data["brutalHeadHitPos"] = *headContact.point;
            if (sourceType == MWMechanics::DamageSourceType::Melee
                && attacker == MWMechanics::getPlayer() && !weapon.isEmpty()
                && weapon.getType() == ESM::REC_WEAP && attackType == ESM::Weapon::AT_Slash)
            {
                const int type = weapon.get<ESM::Weapon>()->mBase->mData.mType;
                if (MWMechanics::getWeaponType(type)->mWeaponClass == ESM::WeaponType::Melee)
                {
                    // The imported slash is DM bigslash_*_L. Its authored impactdir
                    // is (0,-1,0): actor-yaw RIGHT, independent of aim pitch.
                    // Keep the aimed contact ray separate from the force direction.
                    const float yaw = attacker.getRefData().getPosition().rot[2];
                    // A staff's lateral swing sweeps right to left in the Dark Messiah set.
                    const bool leftward = attacker.getClass().getCreatureStats(attacker).mBrutalSlashRight
                        || (MWMechanics::brutalStaffWeapon(type) && MWMechanics::brutalDmFirstPerson());
                    const float sign = leftward ? -1.f : 1.f;
                    data["brutalImpactDirection"] = osg::Vec3f(std::cos(yaw), -std::sin(yaw), 0.f) * sign;
                }
            }
            switch (sourceType)
            {
                case MWMechanics::DamageSourceType::Unspecified:
                    data["sourceType"] = "unspecified";
                    break;
                case MWMechanics::DamageSourceType::Melee:
                    data["sourceType"] = "melee";
                    break;
                case MWMechanics::DamageSourceType::Ranged:
                    data["sourceType"] = "ranged";
                    break;
                case MWMechanics::DamageSourceType::Magical:
                    data["sourceType"] = "magic";
                    break;
            }

            sendLocalEvent(victim, "Hit", data);
        });
    }

    void LuaManager::swordClinchResolved(const MWWorld::Ptr& player, const MWWorld::Ptr& opponent,
        uint64_t id, std::string_view result, std::string_view reason, int clicks, int requiredClicks)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table data = view.newTable();
            data["id"] = id;
            data["result"] = std::string(result);
            data["reason"] = std::string(reason);
            data["clicks"] = clicks;
            data["requiredClicks"] = requiredClicks;
            if (!opponent.isEmpty()) data["opponent"] = LObject(opponent);
            sendLocalEvent(player, "BrutalClinchResolved", data);
        });
    }

    void LuaManager::meleeResolved(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim, const MWWorld::Ptr& weapon,
        uint64_t attackId, const std::string& token, bool powered, bool successful, bool blocked,
        float damage, float strength, const osg::Vec3f& hitPos)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table data = view.newTable();
            data["attackId"] = attackId;
            data["token"] = token;
            data["powered"] = powered;
            data["successful"] = successful;
            data["blocked"] = blocked;
            data["damage"] = damage; // Before Lua armor/difficulty mitigation, never final health loss.
            data["strength"] = strength;
            data["hitPos"] = hitPos;
            if (!victim.isEmpty()) data["target"] = LObject(victim);
            if (!weapon.isEmpty()) data["weapon"] = LObject(weapon);
            sendLocalEvent(attacker, "BrutalMeleeResolved", data);
        });
    }

    void LuaManager::actorExternalImpact(const MWWorld::Ptr& actor, const MWWorld::Ptr& hitObject,
        const osg::Vec3f& position, const osg::Vec3f& normal, float speed)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table data = view.newTable();
            if (!hitObject.isEmpty())
                data["hitObject"] = LObject(hitObject);
            data["hitPos"] = position;
            data["normal"] = normal;
            data["speed"] = speed;
            sendLocalEvent(actor, "BrutalNativeImpact", data);
        });
    }

    void LuaManager::npcKickRagdoll(const MWWorld::Ptr& attacker, const MWWorld::Ptr& target)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table data = view.newTable();
            data["target"] = LObject(target);
            sendLocalEvent(attacker, "BrutalKickRagdoll", data);
        });
    }

    void LuaManager::objectAddedToScene(const MWWorld::Ptr& ptr)
    {
        mObjectLists.objectAddedToScene(ptr); // assigns generated RefNum if it is not set yet.
        mEngineEvents.addToQueue(EngineEvents::OnActive{ getId(ptr) });

        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
        {
            LuaUtil::ScriptIdsWithInitializationData autoStartConf
                = mConfiguration.getLocalConf(getLiveCellRefType(ptr.mRef), ptr.getCellRef().getRefId(), getId(ptr));
            if (!autoStartConf.empty())
            {
                localScripts = createLocalScripts(ptr, std::move(autoStartConf));
                mQueuedAutoStartedScripts.push_back(localScripts->getWeakPointer());
            }
        }
        if (localScripts)
            mActiveLocalScripts.insert(localScripts->getWeakPointer());
    }

    void LuaManager::objectRemovedFromScene(const MWWorld::Ptr& ptr)
    {
        mObjectLists.objectRemovedFromScene(ptr);
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (localScripts)
        {
            // TODO replace with mActiveLocalScripts.erase(localScripts) when we switch to C++23
            auto it = mActiveLocalScripts.find(localScripts);
            if (it != mActiveLocalScripts.end())
                mActiveLocalScripts.erase(it);
            if (!MWBase::Environment::get().getWorldModel()->getPtr(getId(ptr)).isEmpty())
                mEngineEvents.addToQueue(EngineEvents::OnInactive{ getId(ptr) });
        }
    }

    void LuaManager::inputEvent(const InputEvent& event)
    {
        if (!MyGUI::InputManager::getInstance().isModalAny()
            && !MWBase::Environment::get().getWindowManager()->isConsoleMode())
        {
            mInputEvents.push_back(event);
        }
        mMenuInputEvents.push_back(event);
    }

    MWBase::LuaManager::ActorControls* LuaManager::getActorControls(const MWWorld::Ptr& ptr) const
    {
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
            return nullptr;
        return localScripts->getActorControls();
    }

    void LuaManager::addCustomLocalScript(const MWWorld::Ptr& ptr, int scriptId, std::string_view initData)
    {
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
        {
            localScripts = createLocalScripts(ptr);
            localScripts->addAutoStartedScripts();
            if (ptr.isInCell() && MWBase::Environment::get().getWorldScene()->isCellActive(*ptr.getCell()))
            {
                localScripts->setActive(true, false);
                mActiveLocalScripts.insert(localScripts->getWeakPointer());
            }
        }
        localScripts->addCustomScript(scriptId, initData);
    }

    LocalScripts* LuaManager::createLocalScripts(
        const MWWorld::Ptr& ptr, std::optional<LuaUtil::ScriptIdsWithInitializationData> autoStartConf)
    {
        assert(mInitialized);
        std::shared_ptr<LocalScripts> scripts;
        const uint32_t type = getLiveCellRefType(ptr.mRef);
        if (type == ESM::REC_STAT)
            throw std::runtime_error("Lua scripts on static objects are not allowed");
        else if (type == ESM::REC_INTERNAL_PLAYER)
        {
            scripts = std::make_shared<PlayerScripts>(&mLua, LObject(getId(ptr)));
            scripts->setAutoStartConf(mConfiguration.getPlayerConf());
            for (const auto& [name, package] : mPlayerPackages)
                scripts->addPackage(name, package);
        }
        else
        {
            scripts = std::make_shared<LocalScripts>(&mLua, LObject(getId(ptr)), &mScriptTracker);
            if (!autoStartConf.has_value())
                autoStartConf = mConfiguration.getLocalConf(type, ptr.getCellRef().getRefId(), getId(ptr));
            scripts->setAutoStartConf(std::move(*autoStartConf));
            for (const auto& [name, package] : mLocalPackages)
                scripts->addPackage(name, package);
        }
        scripts->setSerializer(mLocalSerializer.get());

        MWWorld::RefData& refData = ptr.getRefData();
        refData.setLuaScripts(std::move(scripts));
        return refData.getLuaScripts();
    }

    void LuaManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
    {
        writer.startRecord(ESM::REC_LUAM);

        writer.writeHNT<double>("LUAW", MWBase::Environment::get().getWorld()->getTimeManager()->getSimulationTime());
        writer.writeFormId(MWBase::Environment::get().getWorldModel()->getLastGeneratedRefNum(), true);
        mConfiguration.write(writer);
        ESM::LuaScripts globalScripts;
        mGlobalScripts.save(globalScripts);
        globalScripts.save(writer);
        mLuaEvents.save(writer);

        writer.endRecord(ESM::REC_LUAM);
    }

    void LuaManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type != ESM::REC_LUAM)
            throw std::runtime_error("ESM::REC_LUAM is expected");

        double simulationTime;
        reader.getHNT(simulationTime, "LUAW");
        MWBase::Environment::get().getWorld()->getTimeManager()->setSimulationTime(simulationTime);
        ESM::FormId lastGenerated = reader.getFormId(true);
        if (lastGenerated.hasContentFile())
            throw std::runtime_error("Last generated RefNum is invalid");
        MWBase::Environment::get().getWorldModel()->setLastGeneratedRefNum(lastGenerated);

        mConfiguration.read(reader);

        // TODO: don't execute scripts right away, it will be necessary in multiplayer where global storage requires
        // initialization. For now just set global storage as active slightly before it would be set by gameLoaded()
        mGlobalStorage.setActive(true);

        ESM::LuaScripts globalScripts;
        globalScripts.load(reader);
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            mLuaEvents.load(view.sol(), reader, mContentFileMapping, mGlobalLoader.get());
        });

        mGlobalScripts.setSavedDataDeserializer(mGlobalLoader.get());
        mGlobalScripts.load(globalScripts);
        mGlobalScriptsStarted = true;
    }

    void LuaManager::saveLocalScripts(const MWWorld::Ptr& ptr, ESM::LuaScripts& data)
    {
        if (ptr.getRefData().getLuaScripts())
            ptr.getRefData().getLuaScripts()->save(data);
        else
            data.mScripts.clear();
    }

    void LuaManager::loadLocalScripts(const MWWorld::Ptr& ptr, const ESM::LuaScripts& data)
    {
        if (data.mScripts.empty())
        {
            if (ptr.getRefData().getLuaScripts())
                ptr.getRefData().setLuaScripts(nullptr);
            return;
        }

        MWBase::Environment::get().getWorldModel()->registerPtr(ptr);
        LocalScripts* scripts = createLocalScripts(ptr);

        scripts->setSerializer(mLocalSerializer.get());
        scripts->setSavedDataDeserializer(mLocalLoader.get());
        scripts->load(data);
    }

    void LuaManager::reloadAllScriptsImpl()
    {
        Log(Debug::Info) << "Reload Lua";

        if (VR::getVR())
            MWVR::VRGUIManager::instance().clearLua();
        LuaUi::clearGameInterface();
        LuaUi::clearMenuInterface();
        LuaUi::clearSettings();
        MWBase::Environment::get().getWindowManager()->setConsoleMode("");
        MWBase::Environment::get().getL10nManager()->dropCache();
        mUiResourceManager.clear();
        mLua.dropScriptCache();
        mInputActions.clear(true);
        mInputTriggers.clear(true);

        ESM::LuaScripts globalData;

        if (mGlobalScriptsStarted)
        {
            mGlobalScripts.setSavedDataDeserializer(mGlobalSerializer.get());
            mGlobalScripts.save(globalData);
            mGlobalStorage.clearTemporaryAndRemoveCallbacks();
        }

        std::unordered_map<ESM::RefNum, ESM::LuaScripts> localData;

        for (const auto& [id, ptr] : MWBase::Environment::get().getWorldModel()->getPtrRegistryView())
        {
            LocalScripts* scripts = ptr.getRefData().getLuaScripts();
            if (scripts == nullptr)
                continue;
            scripts->setSavedDataDeserializer(mLocalSerializer.get());
            ESM::LuaScripts data;
            scripts->save(data);
            localData[id] = std::move(data);
        }

        initConfiguration(true);

        mMenuScripts.removeAllScripts();

        mPlayerStorage.clearTemporaryAndRemoveCallbacks();

        mMenuScripts.addAutoStartedScripts();

        for (const auto& [id, ptr] : MWBase::Environment::get().getWorldModel()->getPtrRegistryView())
        {
            LocalScripts* scripts = ptr.getRefData().getLuaScripts();
            if (scripts == nullptr)
                continue;
            scripts->load(localData[id]);
        }

        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
        {
            if (LocalScripts* scripts = asLocal(ptr))
                scripts->setActive(true);
        }

        if (mGlobalScriptsStarted)
        {
            mGlobalScripts.load(globalData);
        }
    }

    void LuaManager::handleConsoleCommand(
        const std::string& consoleMode, const std::string& command, const MWWorld::Ptr& selectedPtr)
    {
        PlayerScripts* playerScripts = nullptr;
        if (!mPlayer.isEmpty())
            playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        bool processed = mMenuScripts.consoleCommand(consoleMode, command);
        if (playerScripts)
        {
            sol::object selected = sol::nil;
            if (!selectedPtr.isEmpty())
                mLua.protectedCall([&](LuaUtil::LuaView& view) {
                    selected = sol::make_object(view.sol(), LObject(getId(selectedPtr)));
                });
            if (playerScripts->consoleCommand(consoleMode, command, selected))
                processed = true;
        }
        if (!processed)
            MWBase::Environment::get().getWindowManager()->printToConsole(
                "No Lua handlers for console\n", MWBase::WindowManager::sConsoleColor_Error);
    }

    LuaManager::DelayedAction::DelayedAction(LuaUtil::LuaState* state, std::function<void()> fn, std::string_view name)
        : mFn(std::move(fn))
        , mName(name)
    {
        if (Settings::lua().mLuaDebug)
            mCallerTraceback = state->debugTraceback();
    }

    void LuaManager::DelayedAction::apply() const
    {
        try
        {
            mFn();
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "Error in DelayedAction " << mName << ": " << e.what();

            if (mCallerTraceback.empty())
                Log(Debug::Error) << "Set 'lua debug=true' in settings.cfg to enable action tracebacks";
            else
                Log(Debug::Error) << "Caller " << mCallerTraceback;
        }
    }

    void LuaManager::addAction(std::function<void()> action, std::string_view name)
    {
        if (mApplyingDelayedActions)
            throw std::runtime_error("DelayedAction is not allowed to create another DelayedAction");
        mActionQueue.emplace_back(&mLua, std::move(action), name);
    }

    void LuaManager::addTeleportPlayerAction(std::function<void()> action)
    {
        mTeleportPlayerAction = DelayedAction(&mLua, std::move(action), "TeleportPlayer");
    }

    void LuaManager::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        stats.setAttribute(frameNumber, "Lua UsedMemory", static_cast<double>(mLua.getTotalMemoryUsage()));
    }

    std::string LuaManager::formatResourceUsageStats() const
    {
        if (!LuaUtil::LuaState::isProfilerEnabled())
            return "Lua profiler is disabled";

        std::stringstream out;

        constexpr unsigned nameW = 50;
        constexpr int valueW = 12;

        auto outMemSize = [&](size_t bytes) {
            constexpr size_t limit = 10000;
            out << std::right << std::setw(valueW - 3);
            if (bytes < limit)
                out << bytes << " B ";
            else if (bytes < limit * 1024)
                out << (bytes / 1024) << " KB";
            else if (bytes < limit * 1024 * 1024)
                out << (bytes / (1024 * 1024)) << " MB";
            else
                out << (bytes / (1024 * 1024 * 1024)) << " GB";
        };

        const uint64_t smallAllocSize = Settings::lua().mSmallAllocMaxSize;
        out << "Total memory usage:";
        outMemSize(mLua.getTotalMemoryUsage());
        out << "\n";
        out << "LuaUtil::ScriptsContainer count: " << LuaUtil::ScriptsContainer::getInstanceCount() << "\n";
        out << "\n";
        out << "small alloc max size = " << smallAllocSize << " (section [Lua] in settings.cfg)\n";
        out << "Smaller values give more information for the profiler, but increase performance overhead.\n";
        out << "  Memory allocations <= " << smallAllocSize << " bytes:";
        outMemSize(mLua.getSmallAllocMemoryUsage());
        out << " (not tracked)\n";
        out << "  Memory allocations >  " << smallAllocSize << " bytes:";
        outMemSize(mLua.getTotalMemoryUsage() - mLua.getSmallAllocMemoryUsage());
        out << " (see the table below)\n\n";

        using Stats = LuaUtil::ScriptsContainer::ScriptStats;

        std::vector<Stats> activeStats;
        mGlobalScripts.collectStats(activeStats);
        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
        {
            if (LocalScripts* scripts = asLocal(ptr))
                scripts->collectStats(activeStats);
        }

        std::vector<Stats> selectedStats;
        MWWorld::Ptr selectedPtr = MWBase::Environment::get().getWindowManager()->getConsoleSelectedObject();
        LocalScripts* selectedScripts = nullptr;
        if (!selectedPtr.isEmpty())
        {
            selectedScripts = selectedPtr.getRefData().getLuaScripts();
            if (selectedScripts)
                selectedScripts->collectStats(selectedStats);
            out << "Profiled object (selected in the in-game console): " << selectedPtr.toString() << "\n";
        }
        else
            out << "No selected object. Use the in-game console to select an object for detailed profile.\n";
        out << "\n";

        out << "Legend\n";
        out << "  ops:        Averaged number of Lua instruction per frame;\n";
        out << "  memory:     Aggregated size of Lua allocations > " << smallAllocSize << " bytes;\n";
        out << "  [all]:      Sum over all instances of each script;\n";
        out << "  [active]:   Sum over all active (i.e. currently in scene) instances of each script;\n";
        out << "  [inactive]: Sum over all inactive instances of each script;\n";
        out << "  [for selected object]: Only for the object that is selected in the console;\n";
        out << "\n";

        out << std::left;
        out << " " << std::setw(nameW + 2) << "*** Resource usage per script";
        out << std::right;
        out << std::setw(valueW) << "ops";
        out << std::setw(valueW) << "memory";
        out << std::setw(valueW) << "memory";
        out << std::setw(valueW) << "ops";
        out << std::setw(valueW) << "memory";
        out << "\n";
        out << std::left << " " << std::setw(nameW + 2) << "[name]" << std::right;
        out << std::setw(valueW) << "[all]";
        out << std::setw(valueW) << "[active]";
        out << std::setw(valueW) << "[inactive]";
        out << std::setw(valueW * 2) << "[for selected object]";
        out << "\n";

        for (size_t i = 0; i < mConfiguration.size(); ++i)
        {
            bool isGlobal = mConfiguration[i].mFlags & ESM::LuaScriptCfg::sGlobal;
            bool isMenu = mConfiguration[i].mFlags & ESM::LuaScriptCfg::sMenu;

            out << std::left;
            out << " " << std::setw(nameW) << mConfiguration[i].mScriptPath.value();
            if (mConfiguration[i].mScriptPath.value().size() > nameW)
                out << "\n " << std::setw(nameW) << ""; // if path is too long, break line
            out << std::right;
            out << std::setw(valueW) << static_cast<int64_t>(activeStats[i].mAvgInstructionCount);
            outMemSize(static_cast<size_t>(activeStats[i].mMemoryUsage));
            outMemSize(mLua.getMemoryUsageByScriptIndex(static_cast<unsigned>(i))
                - static_cast<uint64_t>(activeStats[i].mMemoryUsage));

            if (isGlobal)
                out << std::setw(valueW * 2) << "NA (global script)";
            else if (isMenu && (!selectedScripts || !selectedScripts->hasScript(static_cast<int>(i))))
                out << std::setw(valueW * 2) << "NA (menu script)";
            else if (selectedPtr.isEmpty())
                out << std::setw(valueW * 2) << "NA (not selected) ";
            else if (!selectedScripts || !selectedScripts->hasScript(static_cast<int>(i)))
                out << std::setw(valueW * 2) << "NA";
            else
            {
                out << std::setw(valueW) << static_cast<int64_t>(selectedStats[i].mAvgInstructionCount);
                outMemSize(static_cast<size_t>(selectedStats[i].mMemoryUsage));
            }
            out << "\n";
        }

        return out.str();
    }
}
