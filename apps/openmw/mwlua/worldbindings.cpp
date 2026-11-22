#include "worldbindings.hpp"
#include "../mwphysics/raycasting.hpp"
#include "../mwrender/animation.hpp"
#include "../mwrender/npcanimation.hpp"
#include "../mwworld/inventorystore.hpp"
#include <cmath>
#include <memory>

#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadalch.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadench.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadmisc.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadprob.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/loadweap.hpp>
#include <components/lua/luastate.hpp>
#include <components/misc/finitevalues.hpp>
#include <components/settings/values.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwworld/action.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/store.hpp"
#include "../mwworld/worldmodel.hpp"

#include "luamanagerimp.hpp"

#include "animationbindings.hpp"
#include "context.hpp"
#include "corebindings.hpp"
#include "mwscriptbindings.hpp"

namespace MWLua
{
    struct CellsStore
    {
    };
}

namespace sol
{
    template <>
    struct is_automagical<MWLua::CellsStore> : std::false_type
    {
    };
}

namespace MWLua
{

    static void checkGameInitialized(LuaUtil::LuaState* lua)
    {
        if (MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_NoGame)
            throw std::runtime_error(
                "This function cannot be used until the game is fully initialized.\n" + lua->debugTraceback());
    }

    static void addWorldTimeBindings(sol::table& api, const Context& context)
    {
        using Misc::FiniteFloat;

        Misc::NotNullPtr<MWBase::World> world = MWBase::Environment::get().getWorld();
        MWWorld::DateTimeManager* timeManager = world->getTimeManager();

        api["advanceTime"] = [context, world](const FiniteFloat hours) {
            if (hours <= 0.0f)
                throw std::runtime_error("Time may only be advanced forward");

            context.mLuaManager->addAction([world, hours] {
                world->advanceTime(hours);
                MWBase::Environment::get().getMechanicsManager()->fastForwardAi();
            });
        };

        api["setGameTimeScale"] = [timeManager](const FiniteFloat scale) { timeManager->setGameTimeScale(scale); };
        api["setSimulationTimeScale"] = [context, timeManager](const FiniteFloat scale) {
            context.mLuaManager->addAction([scale, timeManager] { timeManager->setSimulationTimeScale(scale); });
        };

        api["pause"]
            = [timeManager](sol::optional<std::string_view> tag) { timeManager->pause(tag.value_or("paused")); };
        api["unpause"]
            = [timeManager](sol::optional<std::string_view> tag) { timeManager->unpause(tag.value_or("paused")); };
        api["getPausedTags"] = [timeManager](sol::this_state lua) {
            sol::table res(lua, sol::create);
            for (const std::string& tag : timeManager->getPausedTags())
                res[tag] = tag;
            return res;
        };
    }

    static void addCellGetters(sol::table& api, const Context& context)
    {
        api["getCellByName"] = [](std::string_view name) {
            return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(name, /*forceLoad=*/false) };
        };
        api["getCellById"] = [](std::string_view stringId) {
            return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(
                ESM::RefId::deserializeText(stringId), /*forceLoad=*/false) };
        };
        api["getExteriorCell"] = [](int x, int y, sol::object cellOrName) {
            ESM::RefId worldspace;
            if (cellOrName.is<GCell>())
                worldspace = cellOrName.as<GCell>().mStore->getCell()->getWorldSpace();
            else if (cellOrName.is<std::string_view>() && !cellOrName.as<std::string_view>().empty())
                worldspace = MWBase::Environment::get()
                                 .getWorldModel()
                                 ->getCell(cellOrName.as<std::string_view>())
                                 .getCell()
                                 ->getWorldSpace();
            else
                worldspace = ESM::Cell::sDefaultWorldspaceId;
            return GCell{ &MWBase::Environment::get().getWorldModel()->getExterior(
                ESM::ExteriorCellLocation(x, y, worldspace), /*forceLoad=*/false) };
        };

        const MWWorld::Store<ESM::Cell>* cells3Store = &MWBase::Environment::get().getESMStore()->get<ESM::Cell>();
        const MWWorld::Store<ESM4::Cell>* cells4Store = &MWBase::Environment::get().getESMStore()->get<ESM4::Cell>();
        auto view = context.sol();
        sol::usertype<CellsStore> cells = view.new_usertype<CellsStore>("Cells");
        cells[sol::meta_function::length]
            = [cells3Store, cells4Store](const CellsStore&) { return cells3Store->getSize() + cells4Store->getSize(); };
        cells[sol::meta_function::index]
            = [cells3Store, cells4Store](const CellsStore&, size_t index) -> sol::optional<GCell> {
            if (index > cells3Store->getSize() + cells3Store->getSize() || index == 0)
                return sol::nullopt;

            index--; // Translate from Lua's 1-based indexing.
            if (index < cells3Store->getSize())
            {
                const ESM::Cell* cellRecord = cells3Store->at(index);
                return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(
                    cellRecord->mId, /*forceLoad=*/false) };
            }
            else
            {
                const ESM4::Cell* cellRecord = cells4Store->at(index - cells3Store->getSize());
                return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(
                    cellRecord->mId, /*forceLoad=*/false) };
            }
        };
        cells[sol::meta_function::pairs] = view["ipairsForArray"].template get<sol::function>();
        cells[sol::meta_function::ipairs] = view["ipairsForArray"].template get<sol::function>();
        api["cells"] = CellsStore{};
    }

    sol::table initWorldPackage(const Context& context)
    {
        sol::table api(context.mLua->unsafeState(), sol::create);

        addCoreTimeBindings(api, context);
        addWorldTimeBindings(api, context);
        addCellGetters(api, context);
        api["mwscript"] = initMWScriptBindings(context);
        struct ShieldImpactPlacement
        {
            osg::Vec3f position;
            bool anchored = false;
            bool surfaceHit = false;
            uint64_t anchors = 0;
            osg::Vec3f lastPosition;
            std::string lastKind;
            bool lastExact = false;
            uint64_t exactContacts = 0;
            uint64_t bloodProjections = 0, bloodProjectionFallbacks = 0, bloodProjectionMisses = 0;
            int bloodInputPart = -1, bloodSurfacePart = -1;
        };
        const auto shieldPlacement = std::make_shared<ShieldImpactPlacement>();
        api["spawnBrutalImpact"] = [context, shieldPlacement](std::string kind, Misc::FiniteVec3f position,
            Misc::FiniteVec3f direction, sol::optional<sol::table> options) {
            if (kind != "blood" && kind != "sparks") throw std::runtime_error("Impact kind must be blood or sparks");
            float intensity = 1.f;
            bool power = false;
            bool contactExact = false;
            sol::optional<GObject> actor;
            std::string attachment;
            std::string woundKind;
            int woundPart=-1;
            if (options)
            {
                intensity = float(options->get_or<Misc::FiniteFloat>("intensity", 1.f));
                power = options->get<sol::optional<bool>>("power").value_or(false);
                contactExact = options->get<sol::optional<bool>>("contactExact").value_or(false);
                actor = options->get<sol::optional<GObject>>("actor");
                attachment = options->get<sol::optional<std::string>>("attachment").value_or("");
                woundKind = options->get<sol::optional<std::string>>("woundKind").value_or("");
                woundPart = options->get<sol::optional<int>>("woundPart").value_or(-1);
            }
            if (!attachment.empty() && (attachment != "shield" || kind != "sparks" || !actor))
                throw std::runtime_error("Shield attachment requires sparks and an actor");
            if (!attachment.empty())
            {
                const auto ptr = actor->ptrOrEmpty();
                if (ptr.isEmpty() || !ptr.getClass().isNpc()) return false;
            }
            intensity = std::clamp(intensity, 0.1f, 3.f);
            const auto stats = MWBase::Environment::get().getWorld()->getBrutalImpactStats();
            const bool particles = Settings::game().mBrutalEnabled
                && (kind == "blood" ? Settings::game().mBrutalBloodEffects : Settings::game().mBrutalSparkEffects)
                && stats.at("enabled") && stats.at(kind == "blood" ? "bloodAvailable" : "sparksAvailable");
            const bool marks = Settings::game().mBrutalEnabled && kind == "blood" && actor
                && Settings::game().mNpcBloodDecals;
            if (!particles && !marks) return false;
            context.mLuaManager->addAction([kind=std::move(kind), position=osg::Vec3f(position),
                direction=osg::Vec3f(direction), intensity, power, contactExact, actor, attachment, woundKind, woundPart, shieldPlacement, context]() mutable {
                auto world = MWBase::Environment::get().getWorld();
                osg::Vec3f actualPosition = position;
                osg::Vec3f decalDirection = direction;
                const auto projectBloodSurface = [&](const MWWorld::Ptr& ptr, const osg::Vec3f& point) {
                    osg::Vec3f outward = direction;
                    if (outward.normalize() <= .001f) return false;
                    osg::Vec3f start = point + outward * 128.f;
                    const osg::Vec3f end = point - outward * 128.f;
                    // Hair and carried equipment are not wound surfaces. A stock
                    // damage result can lie behind them, so inspect the actual skin.
                    for (unsigned attempt = 0; attempt < 4; ++attempt)
                    {
                        MWPhysics::RayCastingResult surface;
                        if (!world->castRenderingRay(surface, start, end, true, false, {}, true)
                            || surface.mHitObject != ptr) break;
                        if (surface.mHitPart != ESM::PRT_Hair && surface.mHitPart != ESM::PRT_Weapon
                            && surface.mHitPart != ESM::PRT_Shield)
                        {
                            actualPosition = surface.mHitPos;
                            decalDirection = surface.mHitNormal;
                            woundPart = surface.mHitPart; // the new visual surface owns its part
                            shieldPlacement->bloodSurfacePart = woundPart;
                            ++shieldPlacement->bloodProjections;
                            return true;
                        }
                        start = surface.mHitPos - outward * .5f;
                    }
                    ++shieldPlacement->bloodProjectionMisses;
                    return false;
                };
                if (kind == "blood" && actor && !contactExact && Settings::game().mNpcBloodDecals)
                {
                    const auto ptr = actor->ptrOrEmpty();
                    if (!ptr.isEmpty() && ptr.isInCell() && ptr.getRefData().isEnabled())
                    {
                        // This changes wound presentation only. Stock target,
                        // skill roll and damage remain the original result.
                        shieldPlacement->bloodInputPart = woundPart;
                        projectBloodSurface(ptr, position);
                    }
                }
                if (attachment == "shield" && !contactExact)
                {
                    const auto ptr = actor->ptrOrEmpty();
                    if (ptr.isEmpty() || !ptr.isInCell() || !ptr.getRefData().isEnabled()
                        || !ptr.getRefData().getBaseNode()) return;
                    shieldPlacement->anchored = false;
                    shieldPlacement->surfaceHit = false;
                    const auto& inventory = ptr.getClass().getInventoryStore(ptr);
                    const auto left = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
                    const auto* animation = world->getAnimation(MWWorld::ConstPtr(ptr));
                    if (left != inventory.end() && left->getType() == ESM::Armor::sRecordId
                        && left->get<ESM::Armor>()->mBase->mData.mType == ESM::Armor::Shield && animation)
                    {
                        // This is the exact attachment name used by NpcAnimation for the equipped shield.
                        if (const auto bone = animation->getBoneWorldMatrix("Shield Bone"))
                        {
                            const osg::Vec3f anchor(bone->getTrans());
                            if (std::isfinite(anchor.x()) && std::isfinite(anchor.y()) && std::isfinite(anchor.z()))
                            {
                                osg::Vec3f outward = direction;
                                if (outward.normalize() < 0.001f) outward.set(0,0,1);
                                actualPosition = anchor + outward * 8.f;
                                MWPhysics::RayCastingResult surface;
                                if (world->castRenderingRay(surface, anchor + outward * 120.f,
                                        anchor - outward * 20.f, true, false)
                                    && surface.mHitObject == ptr
                                    && std::isfinite(surface.mHitPos.x())
                                    && std::isfinite(surface.mHitPos.y())
                                    && std::isfinite(surface.mHitPos.z()))
                                {
                                    actualPosition = surface.mHitPos + outward * 2.f;
                                    shieldPlacement->surfaceHit = true;
                                }
                                shieldPlacement->anchored = true;
                                ++shieldPlacement->anchors;
                            }
                        }
                    }
                    shieldPlacement->position = actualPosition;
                }
                shieldPlacement->lastPosition = actualPosition;
                shieldPlacement->lastKind = kind;
                shieldPlacement->lastExact = contactExact;
                if (contactExact) ++shieldPlacement->exactContacts;
                if (Settings::game().mBrutalEnabled
                    && (kind == "blood" ? Settings::game().mBrutalBloodEffects : Settings::game().mBrutalSparkEffects))
                    world->spawnBrutalImpact(kind, actualPosition, direction, intensity, power);
                if(Settings::game().mBrutalEnabled && Settings::game().mNpcBloodDecals && kind=="blood"&&actor)
                {
                    const auto ptr=actor->ptrOrEmpty();
                    if(!ptr.isEmpty()&&ptr.isInCell()&&ptr.getRefData().isEnabled())
                        if(auto* npc=dynamic_cast<MWRender::NpcAnimation*>(world->getAnimation(ptr)))
                        {
                            bool added = npc->addBloodDecal(actualPosition,decalDirection,woundKind,power,woundPart);
                            if (!added && !contactExact)
                                // The stock random bounds point can lie between the
                                // legs or outside the posed skin. Try real torso
                                // surfaces rather than discarding a valid wound.
                                for (const char* bone : { "Bip01 Spine1", "Bip01 Spine2", "Bip01 Pelvis" })
                                    if (const auto anchor = npc->getBoneWorldMatrix(bone))
                                        if (projectBloodSurface(ptr, anchor->getTrans())
                                            && npc->addBloodDecal(actualPosition,decalDirection,woundKind,power,woundPart))
                                        {
                                            added = true;
                                            ++shieldPlacement->bloodProjectionFallbacks;
                                            break;
                                        }
                            if (added)
                                context.mLua->protectedCall([&](LuaUtil::LuaView& view){
                                    sol::table receipt=view.newTable();receipt["data"]=npc->getBloodDecalData();receipt["restored"]=true;
                                    context.mLuaManager->sendLocalEvent(ptr,"BrutalBloodDecalsStored",receipt);
                                });
                        }
                }
            }, "SpawnBrutalImpact");
            return true; // Queued; the renderer enforces the shared live-particle cap.
        };
        api["getBrutalImpactStats"] = [lua=context.mLua, shieldPlacement] {
            sol::table result(lua->unsafeState(), sol::create);
            for (const auto& [key, value] : MWBase::Environment::get().getWorld()->getBrutalImpactStats())
            {
                if (key == "enabled" || key == "bloodAvailable" || key == "sparksAvailable") result[key] = value != 0;
                else result[key] = value;
            }
            result["lastShieldAnchored"] = shieldPlacement->anchored;
            result["lastShieldSurfaceHit"] = shieldPlacement->surfaceHit;
            result["lastShieldPosition"] = shieldPlacement->position;
            result["shieldAnchors"] = shieldPlacement->anchors;
            result["lastContactPosition"] = shieldPlacement->lastPosition;
            result["lastContactKind"] = shieldPlacement->lastKind;
            result["lastContactExact"] = shieldPlacement->lastExact;
            result["exactContacts"] = shieldPlacement->exactContacts;
            result["bloodProjections"] = shieldPlacement->bloodProjections;
            result["bloodProjectionFallbacks"] = shieldPlacement->bloodProjectionFallbacks;
            result["bloodProjectionMisses"] = shieldPlacement->bloodProjectionMisses;
            result["bloodInputPart"] = shieldPlacement->bloodInputPart;
            result["bloodSurfacePart"] = shieldPlacement->bloodSurfacePart;
            return result;
        };
        api["getNpcBloodDecalStats"] = [context](const GObject& object) {
            sol::table result(context.mLua->unsafeState(),sol::create);
            const auto ptr=object.ptrOrEmpty();
            if(!ptr.isEmpty())if(const auto* npc=dynamic_cast<const MWRender::NpcAnimation*>(
                    MWBase::Environment::get().getWorld()->getAnimation(ptr)))
                for(const auto& [key,value]:npc->getBloodDecalStats())result[key]=value;
            return result;
        };
        api["getNpcBloodDecalData"] = [](const GObject& object) {
            const auto ptr=object.ptrOrEmpty();
            if(!ptr.isEmpty())if(const auto* npc=dynamic_cast<const MWRender::NpcAnimation*>(
                    MWBase::Environment::get().getWorld()->getAnimation(ptr)))return npc->getBloodDecalData();
            return std::string();
        };
        api["restoreNpcBloodDecals"] = [context](const GObject& object,std::string data) {
            if(data.size()>400000)throw std::runtime_error("NPC blood data exceeds bounded save size");
            context.mLuaManager->addAction([context,object,data=std::move(data)] {
                const auto ptr=object.ptrOrEmpty();if(ptr.isEmpty()||!ptr.isInCell()||!ptr.getRefData().isEnabled())return;
                if(auto* npc=dynamic_cast<MWRender::NpcAnimation*>(MWBase::Environment::get().getWorld()->getAnimation(ptr)))
                {
                    const bool restored=npc->restoreBloodDecals(data);
                    context.mLua->protectedCall([&](LuaUtil::LuaView& view){
                        sol::table receipt=view.newTable();receipt["restored"]=restored;
                        if(restored)receipt["data"]=npc->getBloodDecalData();
                        context.mLuaManager->sendLocalEvent(ptr,"BrutalBloodDecalsStored",receipt);
                    });
                }
            },"RestoreNpcBloodDecals");
        };
        api["applyNpcKick"] = [context](const GObject& object, Misc::FiniteVec3f value, const GObject& attacker,
                                       sol::optional<bool> knockDown, sol::optional<bool> slideTrip) {
            const osg::Vec3f kick=value;
            if (kick.length2()>1000.f*1000.f) throw std::runtime_error("NPC kick velocity must be <=1000 units/s");
            context.mLuaManager->addAction([object,attacker,kick,knockDown=knockDown.value_or(false),
                                              slideTrip=slideTrip.value_or(false)] {
                const auto ptr=object.ptrOrEmpty(), source=attacker.ptrOrEmpty();
                if (!ptr.isEmpty() && ptr.isInCell() && !source.isEmpty())
                    MWBase::Environment::get().getWorld()->applyNpcKick(ptr,kick,source,knockDown,slideTrip);
            },"ApplyNpcKick");
        };
        // The receipt reports the completed native handoff, rather than queue acceptance.
        api["applyNpcSuperKick"] = [context](const GObject& object, Misc::FiniteVec3f value,
                                            const GObject& attacker, double kickId) {
            const osg::Vec3f kick=value;
            if (kick.length2()>1000.f*1000.f || kick.length2()<1.f)
                throw std::runtime_error("Super kick velocity must be in 1..1000 units/s");
            if (!std::isfinite(kickId) || kickId<1 || kickId>9007199254740991. || std::floor(kickId)!=kickId)
                throw std::runtime_error("Super kick id must be a positive exactly representable integer");
            context.mLuaManager->addAction([context,object,attacker,kick,kickId] {
                const auto ptr=object.ptrOrEmpty(), source=attacker.ptrOrEmpty();
                auto world=MWBase::Environment::get().getWorld();
                bool accepted=false;
                if (!ptr.isEmpty() && ptr.isInCell() && ptr.getRefData().isEnabled()
                    && !source.isEmpty() && source==world->getPlayerPtr() && source.isInCell()
                    && ptr.getCell()==source.getCell() && !source.getClass().getCreatureStats(source).isDead()
                    // Reach ends at the body: a huge creature's centre lies beyond any boot.
                    && (ptr.getRefData().getPosition().asVec3()-source.getRefData().getPosition().asVec3()).length()
                        <=190.f+std::max(world->getHalfExtents(ptr).x(),world->getHalfExtents(ptr).y()))
                    accepted=world->applyNpcSuperKick(ptr,kick,source,static_cast<std::uint64_t>(kickId));
                if (!source.isEmpty()) context.mLua->protectedCall([&](LuaUtil::LuaView& view) {
                    sol::table receipt=view.newTable();receipt["kickId"]=kickId;receipt["accepted"]=accepted;
                    context.mLuaManager->sendLocalEvent(source,"BrutalSuperKickResolved",receipt);
                });
            },"ApplyNpcSuperKick");
        };
        api["getNpcSuperKickState"] = [lua=context.mLua](const GObject& object) -> sol::object {
            const auto ptr=object.ptrOrEmpty();
            if (ptr.isEmpty()) return sol::nil;
            const auto values=MWBase::Environment::get().getWorld()->getNpcSuperKickState(ptr);
            if (values.empty()) return sol::nil;
            sol::table state(lua->unsafeState(),sol::create);
            for (const auto& [key,value]:values) state[key]=value;
            return state;
        };
        api["startNpcRagdoll"] = [context](const GObject& object, Misc::FiniteVec3f value) {
            const osg::Vec3f kick = value;
            if (kick.length2() > 1000.f * 1000.f)
                throw std::runtime_error("Ragdoll velocity change must be <= 1000 units/s");
            const auto initial = object.ptrOrEmpty();
            if (initial.isEmpty() || !MWBase::Environment::get().getWorld()->canStartNpcRagdoll(initial))
                return false;
            context.mLuaManager->addAction([object, kick] {
                const auto ptr = object.ptrOrEmpty();
                if (!ptr.isEmpty())
                    MWBase::Environment::get().getWorld()->startNpcRagdoll(ptr, kick);
            }, "StartNpcRagdoll");
            return true; // accepted for queueing; object/state is checked again by the action
        };
        api["startNpcDecapitation"] = [context](const GObject& object, Misc::FiniteVec3f value,
                                          sol::optional<Misc::FiniteVec3f> direction) {
            const osg::Vec3f kick = value;
            const osg::Vec3f cutDirection = direction ? osg::Vec3f(*direction) : osg::Vec3f();
            if (kick.length2() > 1000.f * 1000.f)
                throw std::runtime_error("Decapitation velocity change must be <= 1000 units/s");
            if (cutDirection.length2() > 1000.f * 1000.f)
                throw std::runtime_error("Decapitation direction exceeds numeric safety bound");
            context.mLuaManager->addAction([object, kick, cutDirection] {
                const auto ptr = object.ptrOrEmpty();
                if (!ptr.isEmpty())
                    MWBase::Environment::get().getWorld()->startNpcDecapitation(ptr, kick, cutDirection);
            }, "StartNpcDecapitation");
        };
        api["startNpcBisection"] = [context](const GObject& object, Misc::FiniteVec3f value,
                                       sol::optional<Misc::FiniteVec3f> direction, sol::optional<std::string> savedPose) {
            const osg::Vec3f kick = value;
            const osg::Vec3f cutDirection = direction ? osg::Vec3f(*direction) : osg::Vec3f();
            if (kick.length2() > 1000.f * 1000.f || cutDirection.length2() > 1000.f * 1000.f)
                throw std::runtime_error("Bisection velocity and direction must be <= 1000 units/s");
            std::string pose = savedPose.value_or("");
            if (!pose.empty() && pose.size() != 60) pose = "!"; // Bounded invalid restore intent; never a fresh launch.
            context.mLuaManager->addAction([object, kick, cutDirection, pose=std::move(pose)] {
                const auto ptr = object.ptrOrEmpty();
                if (!ptr.isEmpty()) MWBase::Environment::get().getWorld()->startNpcBisection(ptr, kick, cutDirection, pose);
            }, "StartNpcBisection");
        };
        api["startNpcLimbSevering"] = [context](const GObject& object, unsigned mask, Misc::FiniteVec3f value,
            sol::optional<Misc::FiniteVec3f> direction, sol::optional<std::string> savedPose) {
            const osg::Vec3f kick = value;
            const osg::Vec3f cutDirection = direction ? osg::Vec3f(*direction) : osg::Vec3f();
            if (!mask || (mask & ~15u) || kick.length2() > 1000.f * 1000.f
                || cutDirection.length2() > 1000.f * 1000.f)
                throw std::runtime_error("Invalid severed-limb mask or velocity");
            std::string pose = savedPose.value_or("");
            if (pose.size() > 2048) throw std::runtime_error("Severed-limb pose exceeds bounded save size");
            context.mLuaManager->addAction([object,mask,kick,cutDirection,pose=std::move(pose)] {
                const auto ptr = object.ptrOrEmpty();
                if (!ptr.isEmpty()) MWBase::Environment::get().getWorld()->startNpcLimbSevering(
                    ptr,mask,kick,cutDirection,pose);
            }, "StartNpcLimbSevering");
        };
        api["getNpcRagdollEnhancementState"] = [lua=context.mLua](const GObject& object) -> sol::object {
            const auto values = MWBase::Environment::get().getWorld()->getNpcRagdollEnhancementState(object.ptr());
            if (values.empty()) return sol::nil;
            sol::table state(lua->unsafeState(), sol::create);
            for (const auto& [name, value] : values) state[name] = value;
            return state;
        };
        api["getNpcRagdollDroppedItems"] = [lua=context.mLua](const GObject& object) {
            sol::table result(lua->unsafeState(), sol::create);
            const auto entries = MWBase::Environment::get().getWorld()->getNpcRagdollDroppedItems(object.ptr());
            int index = 1;
            for (const auto& entry : entries)
            {
                sol::table state(lua->unsafeState(), sol::create);
                state["object"] = GObject(entry.mObject.id());
                state["sourceItemId"] = entry.mSourceObjectId.toString();
                state["sourceRecordId"] = entry.mSourceItemId.serializeText();
                state["identityPreserved"] = entry.mSourceObjectId == entry.mObject.id();
                state["slot"] = entry.mSlot;
                state["initialLinearVelocity"] = entry.mInitialLinearVelocity;
                state["initialAngularVelocity"] = entry.mInitialAngularVelocity;
                state["maxWorldVertexError"] = entry.mMaxWorldVertexError;
                state["sourceCondition"] = entry.mSourceCondition;
                state["initialCondition"] = entry.mInitialCondition;
                state["sourceEnchantmentCharge"] = entry.mSourceEnchantmentCharge;
                state["initialEnchantmentCharge"] = entry.mInitialEnchantmentCharge;
                state["itemDataPreserved"] = entry.mItemDataPreserved;
                result[index++] = state;
            }
            return result;
        };
        api["getReleasedEquipmentState"] = [lua=context.mLua](const GObject& object) -> sol::object {
            const auto ptr = object.ptr();
            const auto data = MWBase::Environment::get().getWorld()->getReleasedEquipmentState(ptr);
            if (data.size() < 5)
                return sol::nil;
            sol::table state(lua->unsafeState(), sol::create);
            state["centerOfMass"] = data[0];
            state["linearVelocity"] = data[1];
            state["angularVelocity"] = data[2];
            state["active"] = data[3].x() > 0.f;
            state["contacts"] = data[3].y();
            state["ignoredGripPairs"] = data[3].z();
            state["worldContacts"] = data[4].x();
            state["ragdollContacts"] = data[4].y();
            // Actual scene-graph pose, distinct from authoritative item.position
            // and Bullet COM; scripts may read this every rendered frame.
            if (const auto* root = ptr.getRefData().getBaseNode())
            {
                const auto rotation = root->getAttitude();
                state["renderPosition"] = osg::Vec3f(root->getPosition());
                state["renderBasisX"] = osg::Vec3f(rotation * osg::Vec3f(1, 0, 0));
                state["renderBasisY"] = osg::Vec3f(rotation * osg::Vec3f(0, 1, 0));
                state["renderBasisZ"] = osg::Vec3f(rotation * osg::Vec3f(0, 0, 1));
                state["renderScale"] = osg::Vec3f(root->getScale());
            }
            return state;
        };
        api["applyNpcRagdollImpact"] = [context](const GObject& object, int part,
            Misc::FiniteVec3f pointValue, Misc::FiniteVec3f impulseValue, double eventId) {
            const osg::Vec3f point = pointValue, impulse = impulseValue;
            if (part < 1 || part > 11 || impulse.length2() > 200000.f * 200000.f
                || !std::isfinite(eventId) || eventId < 1 || eventId > 9007199254740991.0 || std::floor(eventId) != eventId)
                throw std::runtime_error("Ragdoll impact requires part1..11, bounded impulse and a positive integer event ID");
            context.mLuaManager->addAction([object, part, point, impulse, id=std::uint64_t(eventId)] {
                const auto ptr = object.ptrOrEmpty();
                if (!ptr.isEmpty()) MWBase::Environment::get().getWorld()->applyNpcRagdollImpact(ptr, part - 1, point, impulse, id);
            }, "ApplyNpcRagdollImpact");
        };
        api["getNpcRagdollState"] = [lua=context.mLua](const GObject& object) -> sol::object {
            auto world = MWBase::Environment::get().getWorld();
            const auto data = world->getNpcRagdollState(object.ptr());
            if (data.empty())
                return sol::nil;
            sol::table state(lua->unsafeState(), sol::create);
            state["parts"] = data[0].x(); state["jointError"] = data[0].y();
            state["retainedBodyBottom"] = data[0].z();
            const auto dynamics=world->getNpcRagdollDynamics(object.ptr());
            if (dynamics.size()>=8) {
                state["center"]=dynamics[0];state["velocity"]=dynamics[1];state["inheritedVelocity"]=dynamics[2];
                state["initialVelocity"]=dynamics[3];state["impulse"]=dynamics[4];state["contact"]=dynamics[5];
                state["initialPoseError"]=dynamics[6].x();state["maxSpeed"]=dynamics[6].y();state["maxAngularSpeed"]=dynamics[6].z();
                state["livingKnockdown"]=dynamics[7].x()!=0;state["age"]=dynamics[7].y();
                state["initialRotationError"]=dynamics[7].z();
                if (dynamics.size()>8) state["initialMatrixError"]=dynamics[8].x();
                if (dynamics.size()>9) {
                    state["landingCount"]=dynamics[9].x();state["lastFallHeight"]=dynamics[9].y();state["lastFallDamage"]=dynamics[9].z();
                }
                if (dynamics.size()>10) {
                    state["settledTime"]=dynamics[10].x();state["unsupportedTime"]=dynamics[10].y();state["supported"]=dynamics[10].z()!=0;
                }
                if (dynamics.size()>17) {
                    state["headDetached"]=dynamics[11].x()!=0;
                    state["constraintsRemaining"]=dynamics[11].y();
                    state["headSelfCollisionPairs"]=dynamics[11].z();
                    state["detachTransformError"]=dynamics[12].x();
                    state["detachLinearVelocityError"]=dynamics[12].y();
                    state["detachAngularVelocityError"]=dynamics[12].z();
                    state["headPosition"]=dynamics[13];state["headVelocity"]=dynamics[14];
                    state["headAngularVelocity"]=dynamics[15];
                    state["detachHeadVelocity"]=dynamics[16];state["detachHeadAngularVelocity"]=dynamics[17];
                }
                if (dynamics.size()>27) {
                    state["detachBodyVelocityBefore"]=dynamics[18];state["detachBodyVelocityAfter"]=dynamics[19];
                    state["detachBodyTransformError"]=dynamics[20].x();
                    state["detachBodyLinearVelocityError"]=dynamics[20].y();
                    state["detachBodyAngularVelocityError"]=dynamics[20].z();
                    state["detachBodyMomentumBefore"]=dynamics[21];state["detachBodyMomentumAfter"]=dynamics[22];
                    state["detachBodyAngularMomentumBefore"]=dynamics[23];state["detachBodyAngularMomentumAfter"]=dynamics[24];
                    state["detachBodyMass"]=dynamics[25].x();
                    state["detachBodyMomentumError"]=dynamics[25].y();
                    state["detachBodyAngularMomentumError"]=dynamics[25].z();
                    state["detachBodyCenterBefore"]=dynamics[26];state["detachBodyCenterAfter"]=dynamics[27];
                }
                if (dynamics.size()>46) {
                    state["cutTargetVelocity"]=dynamics[28];state["cutHeadVelocityAfter"]=dynamics[29];
                    state["cutHeadAngularVelocityAfter"]=dynamics[30];state["cutRequestedHeadTorque"]=dynamics[31];
                    state["cutHeadAngularDelta"]=dynamics[32];state["cutAllLinearTargetError"]=dynamics[33].x();
                    state["cutHeadAngularExpectedError"]=dynamics[33].y();state["cutRetainedAngularError"]=dynamics[33].z();
                    state["cutEffectiveHeadTorque"]=dynamics[34];state["cutLaunchApplied"]=dynamics[35].x()!=0;
                    state["cutAngularSpeedLimit"]=dynamics[35].y();state["cutUnclampedAngularSpeed"]=dynamics[35].z();
                    sol::table velocities(lua->unsafeState(), sol::create);
                    for (size_t i=36; i<47; ++i) velocities[i-35]=dynamics[i];
                    state["cutPartVelocities"]=std::move(velocities);
                }
            }
            sol::table bones(lua->unsafeState(), sol::create);
            for (size_t i=1; i+1<data.size(); i+=2)
            {
                sol::table bone(lua->unsafeState(), sol::create);
                bone["physics"] = data[i]; bone["rendered"] = data[i+1];
                bones[(i+1)/2] = std::move(bone);
            }
            state["bones"] = std::move(bones);
            if (const auto* animation = dynamic_cast<MWRender::NpcAnimation*>(world->getAnimation(object.ptr())))
            {
                sol::table geometry(lua->unsafeState(), sol::create);
                for (const auto& [name, value] : animation->getDecapitationStats())
                    geometry[name] = value;
                state["decapitationGeometry"] = std::move(geometry);
                sol::table surfaces(lua->unsafeState(), sol::create);
                for (const auto& [name, value] : animation->getDecapitationSurfacesForAudit())
                    surfaces[name] = value;
                state["decapitationSurfaces"] = std::move(surfaces);
                sol::table bisection(lua->unsafeState(), sol::create);
                for (const auto& [name, value] : animation->getBisectionStats()) bisection[name] = value;
                state["bisectionGeometry"] = std::move(bisection);
                sol::table cutSurfaces(lua->unsafeState(), sol::create);
                for (const auto& [name, value] : animation->getBisectionSurfacesForAudit()) cutSurfaces[name] = value;
                state["bisectionSurfaces"] = std::move(cutSurfaces);
                state["limbsMask"] = world->getNpcSeveredLimbsMask(object.ptr());
                sol::table limbs(lua->unsafeState(), sol::create);
                for (const auto& [name, value] : animation->getLimbSeveringStats()) limbs[name] = value;
                state["limbGeometry"] = std::move(limbs);
                sol::table limbSurfaces(lua->unsafeState(), sol::create);
                for (const auto& [name, value] : animation->getLimbSeveringSurfacesForAudit()) limbSurfaces[name] = value;
                state["limbSurfaces"] = std::move(limbSurfaces);
                const auto control = world->getNpcRagdollEnhancementState(object.ptr());
                const auto found = control.find("torsoDetached");
                state["torsoDetached"] = found != control.end() && found->second != 0;
            }
            return state;
        };
        // A player kick that found no living target can still strike a ragdoll corpse or a
        // severed head. Physics changes run on the main thread; the struck corpse receives
        // the local event BrutalRagdollKicked {head, position, direction, part}.
        api["kickNpcRagdoll"] = [context](Misc::FiniteVec3f originValue, Misc::FiniteVec3f forwardValue,
                                    Misc::FiniteFloat reachValue, Misc::FiniteVec3f velocityValue) {
            osg::Vec3f forward = forwardValue;
            forward.z() = 0.f;
            const float reach = reachValue;
            const osg::Vec3f velocity = velocityValue;
            if (forward.normalize() < 1e-3f || reach <= 0.f || reach > 400.f || velocity.length() > 2000.f)
                throw std::runtime_error("Ragdoll kick: horizontal direction, reach 0..400 and speed <= 2000 required");
            context.mLuaManager->addAction([lua = context.mLua, manager = context.mLuaManager,
                                                origin = osg::Vec3f(originValue), forward, reach, velocity] {
                bool head = false;
                osg::Vec3f point;
                std::size_t part = 0;
                // Kick cone matches the living-target cone (dot 0.8) of the mod's kick.
                const MWWorld::Ptr hit = MWBase::Environment::get().getWorld()->kickNpcRagdoll(
                    origin, forward, reach, .8f, velocity, head, point, part);
                if (hit.isEmpty())
                    return;
                lua->protectedCall([&](LuaUtil::LuaView& view) {
                    sol::table data = view.newTable();
                    data["head"] = head;
                    data["position"] = point;
                    data["direction"] = forward;
                    data["part"] = part;
                    manager->sendLocalEvent(hit, "BrutalRagdollKicked", data);
                });
            }, "KickNpcRagdoll");
        };
        api["applyObjectImpulse"] = [context](const GObject& object, Misc::FiniteVec3f value, Misc::FiniteFloat massValue) {
            const osg::Vec3f impulse = value;
            const float mass = massValue;
            if (mass < 0.05f || mass > 500.f || impulse.length2() > mass * mass * 2000.f * 2000.f)
                throw std::runtime_error("Dynamic prop: mass must be 0.05..500 and impulse/mass <= 2000");
            const auto type = object.ptr().getType();
            if (type != ESM::REC_MISC && type != ESM::REC_BOOK && type != ESM::REC_WEAP
                && type != ESM::REC_ARMO && type != ESM::REC_CLOT && type != ESM::REC_ALCH
                && type != ESM::REC_INGR && type != ESM::REC_LIGH && type != ESM::REC_LOCK
                && type != ESM::REC_PROB && type != ESM::REC_REPA)
                throw std::runtime_error("Dynamic prop: a loose inventory object is required");
            context.mLuaManager->addAction([object, impulse, mass] {
                const auto ptr = object.ptrOrEmpty();
                if (!ptr.isEmpty())
                    MWBase::Environment::get().getWorld()->applyObjectImpulse(ptr, impulse, mass);
            }, "ApplyObjectImpulse");
        };

        ObjectLists* objectLists = context.mObjectLists;
        api["activeActors"] = GObjectList{ objectLists->getActorsInScene() };
        api["players"] = GObjectList{ objectLists->getPlayers() };

        api["createObject"] = [lua = context.mLua](std::string_view recordId, sol::optional<int> count) -> GObject {
            checkGameInitialized(lua);
            MWWorld::ManualRef mref(*MWBase::Environment::get().getESMStore(), ESM::RefId::deserializeText(recordId));
            const MWWorld::Ptr& ptr = mref.getPtr();
            ptr.getRefData().disable();
            MWWorld::CellStore& cell = MWBase::Environment::get().getWorldModel()->getDraftCell();
            MWWorld::Ptr newPtr = ptr.getClass().copyToCell(ptr, cell, count.value_or(1));
            return GObject(newPtr);
        };
        api["getObjectByFormId"] = [](std::string_view formIdStr) -> GObject {
            ESM::RefId refId = ESM::RefId::deserializeText(formIdStr);
            if (!refId.is<ESM::FormId>())
                throw std::runtime_error("FormId expected, got " + std::string(formIdStr) + "; use core.getFormId");
            return GObject(*refId.getIf<ESM::FormId>());
        };

        // Creates a new record in the world database.
        api["createRecord"] = sol::overload(
            [lua = context.mLua](const ESM::Activator& activator) -> const ESM::Activator* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(activator);
            },
            [lua = context.mLua](const ESM::Armor& armor) -> const ESM::Armor* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(armor);
            },
            [lua = context.mLua](const ESM::Clothing& clothing) -> const ESM::Clothing* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(clothing);
            },
            [lua = context.mLua](const ESM::Book& book) -> const ESM::Book* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(book);
            },
            [lua = context.mLua](const ESM::Enchantment& enchantment) -> const ESM::Enchantment* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(enchantment);
            },
            [lua = context.mLua](const ESM::Miscellaneous& misc) -> const ESM::Miscellaneous* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(misc);
            },
            [lua = context.mLua](const ESM::Potion& potion) -> const ESM::Potion* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(potion);
            },
            [lua = context.mLua](const ESM::Probe& probe) -> const ESM::Probe* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(probe);
            },
            [lua = context.mLua](const ESM::Spell& spell) -> const ESM::Spell* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(spell);
            },
            [lua = context.mLua](const ESM::Static& stat) -> const ESM::Static* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(stat);
            },
            [lua = context.mLua](const ESM::NPC& npc) -> const ESM::NPC* {
                checkGameInitialized(lua);
                if (npc.mId.empty())
                    return MWBase::Environment::get().getESMStore()->insert(npc);
                ESM::NPC copy = npc;
                copy.mId = {};
                return MWBase::Environment::get().getESMStore()->insert(copy);
            },
            [lua = context.mLua](const ESM::Creature& crea) -> const ESM::Creature* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(crea);
            },
            [lua = context.mLua](const ESM::Container& cont) -> const ESM::Container* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(cont);
            },
            [lua = context.mLua](const ESM::Door& cont) -> const ESM::Door* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(cont);
            },
            [lua = context.mLua](const ESM::Weapon& weapon) -> const ESM::Weapon* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(weapon);
            },
            [lua = context.mLua](const ESM::Light& light) -> const ESM::Light* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(light);
            });

        api["_runStandardActivationAction"] = [context](const GObject& object, const GObject& actor) {
            if (!object.ptr().getRefData().activate())
                return;
            context.mLuaManager->addAction(
                [object, actor] {
                    const MWWorld::Ptr& objPtr = object.ptr();
                    const MWWorld::Ptr& actorPtr = actor.ptr();
                    objPtr.getClass().activate(objPtr, actorPtr)->execute(actorPtr);
                },
                "_runStandardActivationAction");
        };
        api["_runStandardUseAction"] = [context](const GObject& object, const GObject& actor, bool force) {
            context.mLuaManager->addAction(
                [object, actor, force] {
                    const MWWorld::Ptr& actorPtr = actor.ptr();
                    const MWWorld::Ptr& objectPtr = object.ptr();
                    if (actorPtr == MWBase::Environment::get().getWorld()->getPlayerPtr())
                        MWBase::Environment::get().getWindowManager()->useItem(objectPtr, force);
                    else
                    {
                        std::unique_ptr<MWWorld::Action> action = objectPtr.getClass().use(objectPtr, force);
                        action->execute(actorPtr, true);
                    }
                },
                "_runStandardUseAction");
        };

        api["vfx"] = initWorldVfxBindings(context);

        return LuaUtil::makeReadOnly(api);
    }
}
