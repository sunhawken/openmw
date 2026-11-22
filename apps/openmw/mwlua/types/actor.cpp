#include "../../mwphysics/collisiontype.hpp"
#include <limits>
#include "../../mwphysics/creaturerig.hpp"
#include "types.hpp"

#include <array>
#include <cmath>
#include <components/esm3/loadweap.hpp>

#include <components/detournavigator/agentbounds.hpp>
#include <components/lua/luastate.hpp>
#include <components/misc/finitevalues.hpp>
#include <components/settings/values.hpp>

#include "apps/openmw/mwbase/environment.hpp"
#include "apps/openmw/mwbase/mechanicsmanager.hpp"
#include "apps/openmw/mwbase/windowmanager.hpp"
#include "apps/openmw/mwbase/world.hpp"
#include "apps/openmw/mwmechanics/actorutil.hpp"
#include "apps/openmw/mwmechanics/brutaloptions.hpp"
#include "apps/openmw/mwmechanics/weapontype.hpp"
#include "apps/openmw/mwmechanics/creaturestats.hpp"
#include "apps/openmw/mwmechanics/drawstate.hpp"
#include "apps/openmw/mwphysics/raycasting.hpp"
#include "apps/openmw/mwrender/animation.hpp"
#include "apps/openmw/mwrender/camera.hpp"
#include "apps/openmw/mwrender/npcanimation.hpp"
#include "apps/openmw/mwworld/class.hpp"
#include "apps/openmw/mwworld/inventorystore.hpp"
#include "apps/openmw/mwworld/worldmodel.hpp"

#include "../localscripts.hpp"
#include "../luamanagerimp.hpp"
#include "../magicbindings.hpp"
#include "../stats.hpp"

namespace MWLua
{
    using EquipmentItem = std::variant<std::string, ObjectId>;
    using Equipment = std::map<int, EquipmentItem>;
    static constexpr int sAnySlot = -1;

    static std::pair<MWWorld::ContainerStoreIterator, bool> findInInventory(
        MWWorld::ContainerStore& store, const EquipmentItem& item, int slot, bool isInventoryStore)
    {
        auto oldIt = slot != sAnySlot && isInventoryStore ? static_cast<MWWorld::InventoryStore&>(store).getSlot(slot)
                                                          : store.end();
        MWWorld::Ptr itemPtr;

        if (std::holds_alternative<ObjectId>(item))
        {
            itemPtr = MWBase::Environment::get().getWorldModel()->getPtr(std::get<ObjectId>(item));
            if (oldIt != store.end() && *oldIt == itemPtr)
                return { oldIt, true }; // already equipped
            if (itemPtr.isEmpty() || itemPtr.getCellRef().getCount() == 0 || itemPtr.getContainerStore() != &store)
            {
                Log(Debug::Warning) << "Object" << std::get<ObjectId>(item).toString() << " is not in inventory";
                return { store.end(), false };
            }
        }
        else
        {
            const auto& stringId = std::get<std::string>(item);
            ESM::RefId recordId = ESM::RefId::deserializeText(stringId);
            if (oldIt != store.end() && oldIt->getCellRef().getRefId() == recordId)
                return { oldIt, true }; // already equipped
            itemPtr = store.search(recordId);
            if (itemPtr.isEmpty() || itemPtr.getCellRef().getCount() == 0)
            {
                Log(Debug::Warning) << "There is no object with recordId='" << stringId << "' in inventory";
                return { store.end(), false };
            }
        }

        // TODO: Refactor InventoryStore to accept Ptr and get rid of this linear search.
        MWWorld::ContainerStoreIterator it = std::find(store.begin(), store.end(), itemPtr);
        if (it == store.end()) // should never happen
            throw std::logic_error("Item not found in container");

        return { it, false };
    }

    static void setEquipment(const MWWorld::Ptr& actor, const Equipment& equipment)
    {
        bool isPlayer = actor == MWBase::Environment::get().getWorld()->getPlayerPtr();
        MWWorld::InventoryStore& store = actor.getClass().getInventoryStore(actor);
        std::array<bool, MWWorld::InventoryStore::Slots> usedSlots;
        std::fill(usedSlots.begin(), usedSlots.end(), false);

        auto tryEquipToSlot = [&store, &usedSlots, isPlayer](int slot, const EquipmentItem& item) -> bool {
            auto [it, alreadyEquipped] = findInInventory(store, item, slot, true);
            if (alreadyEquipped)
                return true;
            if (it == store.end())
                return false;
            MWWorld::Ptr itemPtr = *it;

            auto [allowedSlots, _] = itemPtr.getClass().getEquipmentSlots(itemPtr);
            bool requestedSlotIsAllowed
                = std::find(allowedSlots.begin(), allowedSlots.end(), slot) != allowedSlots.end();
            if (!requestedSlotIsAllowed)
            {
                auto firstAllowed
                    = std::find_if(allowedSlots.begin(), allowedSlots.end(), [&](int s) { return !usedSlots[s]; });
                if (firstAllowed == allowedSlots.end())
                {
                    Log(Debug::Warning) << "No suitable slot for " << itemPtr.toString();
                    return false;
                }
                slot = *firstAllowed;
            }

            bool skipEquip = false;

            if (isPlayer)
            {
                const ESM::RefId& script = itemPtr.getClass().getScript(itemPtr);
                if (!script.empty())
                {
                    MWScript::Locals& locals = itemPtr.getRefData().getLocals();
                    locals.setVarByInt(script, "onpcequip", 1);
                    skipEquip = locals.getIntVar(script, "pcskipequip") == 1;
                }
            }

            if (!skipEquip)
                store.equip(slot, it);

            return requestedSlotIsAllowed; // return true if equipped to requested slot and false if slot was changed
        };

        for (int slot = 0; slot < MWWorld::InventoryStore::Slots; ++slot)
        {
            auto oldIt = store.getSlot(slot);
            auto newIt = equipment.find(slot);
            if (newIt == equipment.end())
            {
                if (oldIt != store.end())
                    store.unequipSlot(slot);
                continue;
            }
            if (tryEquipToSlot(slot, newIt->second))
                usedSlots[slot] = true;
        }
        for (const auto& [slot, item] : equipment)
            if (slot >= MWWorld::InventoryStore::Slots)
                tryEquipToSlot(sAnySlot, item);
    }

    static void setSelectedEnchantedItem(const MWWorld::Ptr& actor, const EquipmentItem& item)
    {
        MWWorld::ContainerStore& contStore = actor.getClass().getContainerStore(actor);
        // We're not passing in a specific slot, so ignore the already equipped return value
        auto [it, _] = findInInventory(contStore, item, sAnySlot, actor.getClass().hasInventoryStore(actor));
        if (it == contStore.end())
            return;
        if (!actor.getClass().hasInventoryStore(actor))
        {
            contStore.setSelectedEnchantItem(it);
            return;
        }

        MWWorld::InventoryStore& store = actor.getClass().getInventoryStore(actor);
        MWWorld::Ptr itemPtr = *it;

        // Equip the item if applicable
        auto slots = itemPtr.getClass().getEquipmentSlots(itemPtr);
        if (!slots.first.empty())
        {
            bool alreadyEquipped = false;
            for (auto slot : slots.first)
            {
                if (store.getSlot(slot) == it)
                    alreadyEquipped = true;
            }

            if (!alreadyEquipped)
            {
                MWBase::Environment::get().getWindowManager()->useItem(itemPtr);
                // make sure that item was successfully equipped
                if (!store.isEquipped(itemPtr))
                    return;
            }
        }
        store.setSelectedEnchantItem(it);
        // to reset WindowManager::mSelectedSpell immediately
        MWBase::Environment::get().getWindowManager()->setSelectedEnchantItem(*it);
    }

    void addActorBindings(sol::table actor, const Context& context)
    {
        sol::state_view lua = context.sol();
        actor["STANCE"]
            = LuaUtil::makeStrictReadOnly(LuaUtil::tableFromPairs<std::string_view, MWMechanics::DrawState>(lua,
                {
                    { "Nothing", MWMechanics::DrawState::Nothing },
                    { "Weapon", MWMechanics::DrawState::Weapon },
                    { "Spell", MWMechanics::DrawState::Spell },
                }));
        actor["EQUIPMENT_SLOT"] = LuaUtil::makeStrictReadOnly(LuaUtil::tableFromPairs<std::string_view, int>(lua,
            { { "Helmet", MWWorld::InventoryStore::Slot_Helmet }, { "Cuirass", MWWorld::InventoryStore::Slot_Cuirass },
                { "Greaves", MWWorld::InventoryStore::Slot_Greaves },
                { "LeftPauldron", MWWorld::InventoryStore::Slot_LeftPauldron },
                { "RightPauldron", MWWorld::InventoryStore::Slot_RightPauldron },
                { "LeftGauntlet", MWWorld::InventoryStore::Slot_LeftGauntlet },
                { "RightGauntlet", MWWorld::InventoryStore::Slot_RightGauntlet },
                { "Boots", MWWorld::InventoryStore::Slot_Boots }, { "Shirt", MWWorld::InventoryStore::Slot_Shirt },
                { "Pants", MWWorld::InventoryStore::Slot_Pants }, { "Skirt", MWWorld::InventoryStore::Slot_Skirt },
                { "Robe", MWWorld::InventoryStore::Slot_Robe }, { "LeftRing", MWWorld::InventoryStore::Slot_LeftRing },
                { "RightRing", MWWorld::InventoryStore::Slot_RightRing },
                { "Amulet", MWWorld::InventoryStore::Slot_Amulet }, { "Belt", MWWorld::InventoryStore::Slot_Belt },
                { "CarriedRight", MWWorld::InventoryStore::Slot_CarriedRight },
                { "CarriedLeft", MWWorld::InventoryStore::Slot_CarriedLeft },
                { "Ammunition", MWWorld::InventoryStore::Slot_Ammunition } }));

        actor["getStance"] = [](const Object& o) {
            const MWWorld::Class& cls = o.ptr().getClass();
            if (cls.isActor())
                return cls.getCreatureStats(o.ptr()).getDrawState();
            else
                throw std::runtime_error("Actor expected");
        };
        actor["stance"] = actor["getStance"]; // for compatibility; should be removed later
        actor["setStance"] = [](const SelfObject& self, int stance) {
            const MWWorld::Class& cls = self.ptr().getClass();
            if (!cls.isActor())
                throw std::runtime_error("Actor expected");
            auto& stats = cls.getCreatureStats(self.ptr());
            if (stance != static_cast<int>(MWMechanics::DrawState::Nothing)
                && stance != static_cast<int>(MWMechanics::DrawState::Weapon)
                && stance != static_cast<int>(MWMechanics::DrawState::Spell))
            {
                throw std::runtime_error("Incorrect stance");
            }
            MWMechanics::DrawState newDrawState = static_cast<MWMechanics::DrawState>(stance);
            if (stats.getDrawState() == newDrawState)
                return;
            if (newDrawState == MWMechanics::DrawState::Spell)
            {
                bool hasSelectedSpell;
                if (self.ptr() == MWBase::Environment::get().getWorld()->getPlayerPtr())
                    // For the player selecting spell in UI doesn't change selected spell in CreatureStats (was
                    // implemented this way to prevent changing spell during casting, probably should be refactored), so
                    // we have to handle the player separately.
                    hasSelectedSpell = !MWBase::Environment::get().getWindowManager()->getSelectedSpell().empty();
                else
                    hasSelectedSpell = !stats.getSpells().getSelectedSpell().empty();
                if (!hasSelectedSpell)
                {
                    const MWWorld::ContainerStore& store = cls.getContainerStore(self.ptr());
                    if (store.getSelectedEnchantItem() == store.end())
                        return; // No selected spell and no selected enchanted item; can't use magic stance.
                }
            }
            MWBase::MechanicsManager* mechanics = MWBase::Environment::get().getMechanicsManager();
            // We want to interrupt animation only if attack is preparing, but still is not triggered.
            // Otherwise we will get a "speedshooting" exploit, when player can skip reload animation by hitting "Toggle
            // Weapon" key twice.
            if (mechanics->isAttackPreparing(self.ptr()))
                stats.setAttackingOrSpell(false); // interrupt attack
            else if (mechanics->isAttackingOrSpell(self.ptr()))
                return; // can't be interrupted; ignore setStance
            stats.setDrawState(newDrawState);
        };

        actor["getSelectedEnchantedItem"] = [](sol::this_state thisState, const Object& o) -> sol::object {
            const MWWorld::Ptr& ptr = o.ptr();
            if (!ptr.getClass().isActor())
                return sol::nil;
            MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
            auto it = store.getSelectedEnchantItem();
            if (it == store.end())
                return sol::nil;
            MWBase::Environment::get().getWorldModel()->registerPtr(*it);
            if (o.isGObject())
                return sol::make_object(thisState, GObject(*it));
            else
                return sol::make_object(thisState, LObject(*it));
        };
        actor["setSelectedEnchantedItem"] = [context](const SelfObject& obj, const sol::object& item) {
            const MWWorld::Ptr& ptr = obj.ptr();
            if (!ptr.getClass().isActor())
                return;

            EquipmentItem ei;
            if (item.is<Object>())
            {
                ei = LuaUtil::cast<Object>(item).id();
            }
            else
            {
                ei = LuaUtil::cast<std::string>(item);
            }
            context.mLuaManager->addAction(
                [obj = Object(ptr), ei = std::move(ei)] { setSelectedEnchantedItem(obj.ptr(), ei); },
                "setSelectedEnchantedItemAction");
        };

        actor["canMove"] = [](const Object& o) {
            const MWWorld::Class& cls = o.ptr().getClass();
            return cls.getMaxSpeed(o.ptr()) > 0;
        };
        actor["getRunSpeed"] = [](const Object& o) {
            const MWWorld::Class& cls = o.ptr().getClass();
            return cls.getRunSpeed(o.ptr());
        };
        actor["getWalkSpeed"] = [](const Object& o) {
            const MWWorld::Class& cls = o.ptr().getClass();
            return cls.getWalkSpeed(o.ptr());
        };
        actor["getCurrentSpeed"] = [](const Object& o) {
            const MWWorld::Class& cls = o.ptr().getClass();
            return cls.getCurrentSpeed(o.ptr());
        };

        // for compatibility; should be removed later
        actor["runSpeed"] = actor["getRunSpeed"];
        actor["walkSpeed"] = actor["getWalkSpeed"];
        actor["currentSpeed"] = actor["getCurrentSpeed"];

        actor["isOnGround"]
            = [](const LObject& o) { return MWBase::Environment::get().getWorld()->isOnGround(o.ptr()); };
        actor["isSwimming"]
            = [](const LObject& o) { return MWBase::Environment::get().getWorld()->isSwimming(o.ptr()); };

        actor["inventory"] = sol::overload([](const LObject& o) { return Inventory<LObject>{ o }; },
            [](const GObject& o) { return Inventory<GObject>{ o }; });
        auto getAllEquipment = [](sol::this_state thisState, const Object& o) {
            const MWWorld::Ptr& ptr = o.ptr();
            sol::table equipment(thisState, sol::create);
            if (!ptr.getClass().hasInventoryStore(ptr))
                return equipment;

            MWWorld::InventoryStore& store = ptr.getClass().getInventoryStore(ptr);
            for (int slot = 0; slot < MWWorld::InventoryStore::Slots; ++slot)
            {
                auto it = store.getSlot(slot);
                if (it == store.end())
                    continue;
                MWBase::Environment::get().getWorldModel()->registerPtr(*it);
                if (o.isGObject())
                    equipment[slot] = sol::make_object(thisState, GObject(*it));
                else
                    equipment[slot] = sol::make_object(thisState, LObject(*it));
            }
            return equipment;
        };
        auto getEquipmentFromSlot = [](sol::this_state thisState, const Object& o, int slot) -> sol::object {
            const MWWorld::Ptr& ptr = o.ptr();
            if (!ptr.getClass().hasInventoryStore(ptr))
                return sol::nil;
            MWWorld::InventoryStore& store = ptr.getClass().getInventoryStore(ptr);
            auto it = store.getSlot(slot);
            if (it == store.end())
                return sol::nil;
            MWBase::Environment::get().getWorldModel()->registerPtr(*it);
            if (o.isGObject())
                return sol::make_object(thisState, GObject(*it));
            else
                return sol::make_object(thisState, LObject(*it));
        };
        actor["getEquipment"] = sol::overload(getAllEquipment, getEquipmentFromSlot);
        actor["equipment"] = actor["getEquipment"]; // for compatibility; should be removed later
        actor["hasEquipped"] = [](const Object& o, const Object& item) {
            const MWWorld::Ptr& ptr = o.ptr();
            if (!ptr.getClass().hasInventoryStore(ptr))
                return false;
            MWWorld::InventoryStore& store = ptr.getClass().getInventoryStore(ptr);
            return store.isEquipped(item.ptr());
        };
        actor["setEquipment"] = [context](const SelfObject& obj, const sol::table& equipment) {
            const MWWorld::Ptr& ptr = obj.ptr();
            if (!ptr.getClass().hasInventoryStore(ptr))
            {
                if (!equipment.empty())
                    throw std::runtime_error(obj.toString() + " has no equipment slots");
                return;
            }
            Equipment eqp;
            for (auto& [key, value] : equipment)
            {
                int slot = LuaUtil::cast<int>(key);
                if (value.is<Object>())
                    eqp[slot] = LuaUtil::cast<Object>(value).id();
                else
                    eqp[slot] = LuaUtil::cast<std::string>(value);
            }
            context.mLuaManager->addAction(
                [obj = Object(ptr), eqp = std::move(eqp)] { setEquipment(obj.ptr(), eqp); }, "SetEquipmentAction");
        };
        actor["getPathfindingAgentBounds"] = [](sol::this_state thisState, const LObject& o) {
            const DetourNavigator::AgentBounds agentBounds
                = MWBase::Environment::get().getWorld()->getPathfindingAgentBounds(o.ptr());
            sol::table result(thisState, sol::create);
            result["shapeType"] = agentBounds.mShapeType;
            result["halfExtents"] = agentBounds.mHalfExtents;
            return result;
        };
        actor["isInActorsProcessingRange"] = [](const Object& o) {
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            const auto& target = o.ptr();
            if (target == player)
                return true;

            if (!target.getClass().isActor())
                throw std::runtime_error("Actor expected");

            if (target.getCell()->getCell()->getWorldSpace() != player.getCell()->getCell()->getWorldSpace())
                return false;

            const int actorsProcessingRange = Settings::game().mActorsProcessingRange;
            const osg::Vec3f playerPos = player.getRefData().getPosition().asVec3();

            const float dist = (playerPos - target.getRefData().getPosition().asVec3()).length2();
            return dist <= (actorsProcessingRange * actorsProcessingRange);
        };

        actor["isDead"] = [](const Object& o) {
            const auto& target = o.ptr();
            return target.getClass().getCreatureStats(target).isDead();
        };

        actor["isDeathFinished"] = [](const Object& o) {
            const auto& target = o.ptr();
            return target.getClass().getCreatureStats(target).isDeathAnimationFinished();
        };

        actor["getEncumbrance"] = [](const Object& object) -> float {
            const MWWorld::Ptr ptr = object.ptr();
            return ptr.getClass().getEncumbrance(ptr);
        };

        actor["getCapacity"] = [](const Object& object) -> float {
            const MWWorld::Ptr ptr = object.ptr();
            return ptr.getClass().getCapacity(ptr);
        };

        actor["getBarterGold"] = [](const Object& object) -> int {
            const MWWorld::Ptr ptr = object.ptr();
            return ptr.getClass().getCreatureStats(ptr).getGoldPool();
        };

        actor["setBarterGold"] = [context](const Object& object, int gold) {
            if (gold < 0)
                throw std::runtime_error("Barter gold must be positive");
            if (!object.isGObject() && !object.isSelfObject())
                throw std::runtime_error("Can only be used in global scripts or in local scripts on self.");
            context.mLuaManager->addAction(
                [obj = Object(object), gold] {
                    const MWWorld::Ptr ptr = obj.ptr();
                    ptr.getClass().getCreatureStats(ptr).setGoldPool(gold);
                },
                "SetBarterGoldAction");
        };

        actor["setAdrenalineEffectActive"] = [context](const SelfObject& self, bool active) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("Adrenaline effect state requires player SELF");
            context.mLuaManager->addAction([object = Object(self), active] {
                const auto ptr = object.ptrOrEmpty();
                if (ptr.isEmpty()) return;
                auto& stats = ptr.getClass().getCreatureStats(ptr);
                stats.mBrutalAdrenalineEffectActive = active && !stats.isDead();
            }, "AdrenalineEffectState");
        };
        actor["isAdrenalineEffectActive"] = [](const Object& object) {
            const auto ptr = object.ptr();
            return ptr.getClass().isActor() && ptr.getClass().getCreatureStats(ptr).mBrutalAdrenalineEffectActive;
        };

        // Keep requested values separately: a master toggle must not erase component preferences.
        // The permanent Lua section remains authoritative across loads.
        static std::map<std::string, bool> brutalOptions;
        const auto initialiseOptions = [] {
            if (!brutalOptions.empty()) return;
            const auto& g = Settings::game();
            brutalOptions = {
                {"enabled", g.mBrutalEnabled}, {"dualWield", MWMechanics::sBrutalDualWield}, {"adrenalineHud", g.mBrutalAdrenalineHud}, {"classicFirstPerson", g.mBrutalClassicFirstPerson},
                {"classicNpcAnimations", g.mBrutalClassicNpcAnimations}, {"firstPersonBody", g.mBrutalFirstPersonBody},
                {"dismemberment", g.mBrutalDismemberment}, {"npcCombatTweaks", g.mDarkMessiahNpcCombat},
                {"guaranteedHits", g.mMeleeContactAlwaysHits}, {"ragdoll", g.mBrutalRagdoll},
                {"ragdollEnhancements", g.mRagdollEnhancement}, {"npcStandup", g.mDarkMessiahStandUp},
                {"ragdollSoftStops", g.mRagdollSoftStops}, {"ragdollNeutralPose", g.mRagdollNeutralPose},
                {"ragdollLimbResponse", g.mRagdollLimbResponse}, {"ragdollSelfCollision", g.mRagdollSelfCollision}, {"dynamicProps", g.mBrutalDynamicProps},
                {"equipmentPhysics", g.mBrutalEquipmentPhysics}, {"meleeGeometry", g.mBrutalMeleeGeometry},
                {"impactEffects", g.mBrutalImpactEffects}, {"bloodDecals", g.mNpcBloodDecals},
                {"kineticMovement", g.mKineticPlayerMovement}, {"activeBlocking", g.mPlayerActiveShieldBlock},
                {"swordParry", g.mDarkMessiahSwordParry}, {"comboAttacks", g.mDarkMessiahComboAttacks},
                {"npcStagger", g.mNpcHurtHealthFraction > 0.f}, {"hitRecovery", !g.mPlayerHitRecovery},
                {"stamina", g.mBrutalStamina}, {"wallBounce", g.mBrutalWallBounce},
                {"controlsEnabled", g.mBrutalControlsEnabled}, {"slide", g.mBrutalSlide}, {"clinch", g.mBrutalClinch}, {"fastAttacks", g.mBrutalFastAttacks},
                {"combatSounds", g.mBrutalCombatSounds}, {"bloodEffects", g.mBrutalBloodEffects},
                {"sparkEffects", g.mBrutalSparkEffects}, {"kickAlwaysRagdoll", g.mBrutalKickAlwaysRagdoll}
            };
        };
        actor["getBrutalOption"] = [initialiseOptions](std::string name) -> bool {
            initialiseOptions();
            if (!brutalOptions.contains(name)) throw std::runtime_error("Unknown Brutal option: " + name);
            if (name == "enabled") return brutalOptions.at(name);
            if (!brutalOptions.at("enabled")) return name == "classicFirstPerson" || name == "classicNpcAnimations";
            if (name == "kickAlwaysRagdoll") return brutalOptions.at("ragdoll") && brutalOptions.at(name);
            return brutalOptions.at(name);
        };
        actor["setBrutalOption"] = [context, initialiseOptions](const SelfObject& self, std::string name, bool value) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("Brutal options require player SELF");
            context.mLuaManager->addAction([name=std::move(name), value, initialiseOptions] {
                initialiseOptions();
                if (!brutalOptions.contains(name)) throw std::runtime_error("Unknown Brutal option: " + name);
                if (brutalOptions.at(name) == value) return;
                brutalOptions[name] = value;
                auto& g = Settings::game();
                const bool enabled = brutalOptions.at("enabled");
                const auto on = [enabled](const char* key) { return enabled && brutalOptions.at(key); };
                const bool oldDual = MWMechanics::brutalDualWield();
                const bool oldClassic = g.mBrutalClassicFirstPerson;
                const bool oldBody = g.mBrutalFirstPersonBody;
                const bool oldNpc = g.mBrutalClassicNpcAnimations;
                const bool oldBlock = g.mPlayerActiveShieldBlock;
                const bool oldCombo = g.mDarkMessiahComboAttacks;
                const bool oldFast = g.mBrutalFastAttacks;
                const bool oldBlood = g.mNpcBloodDecals;
                const bool oldMovement = g.mKineticPlayerMovement;
                const bool oldSlide = g.mBrutalSlide;
                g.mBrutalEnabled.set(enabled);
                MWMechanics::sBrutalDualWield = on("dualWield");
                g.mBrutalAdrenalineHud.set(on("adrenalineHud"));
                g.mBrutalClassicFirstPerson.set(!enabled || brutalOptions.at("classicFirstPerson"));
                g.mBrutalClassicNpcAnimations.set(!enabled || brutalOptions.at("classicNpcAnimations"));
                g.mBrutalFirstPersonBody.set(on("firstPersonBody"));
                g.mFirstPersonFullBody.set(false); // Body awareness and arms have independent owners.
                g.mBrutalDismemberment.set(on("dismemberment"));
                g.mDarkMessiahNpcCombat.set(on("npcCombatTweaks"));
                g.mMeleeContactAlwaysHits.set(on("guaranteedHits"));
                g.mBrutalRagdoll.set(on("ragdoll"));
                g.mRagdollEnhancement.set(on("ragdoll") && on("ragdollEnhancements"));
                g.mBrutalKickAlwaysRagdoll.set(on("kickAlwaysRagdoll") && on("ragdoll"));
                g.mDarkMessiahStandUp.set(on("npcStandup") && on("ragdoll") && !g.mBrutalClassicNpcAnimations);
                g.mRagdollSoftStops.set(on("ragdollSoftStops"));
                g.mRagdollNeutralPose.set(on("ragdollNeutralPose"));
                g.mRagdollLimbResponse.set(on("ragdollLimbResponse"));
                g.mRagdollSelfCollision.set(on("ragdollSelfCollision"));
                g.mBrutalDynamicProps.set(on("dynamicProps"));
                g.mBrutalEquipmentPhysics.set(on("equipmentPhysics"));
                g.mBrutalMeleeGeometry.set(on("meleeGeometry"));
                g.mBrutalImpactEffects.set(on("impactEffects"));
                g.mBrutalBloodEffects.set(on("bloodEffects"));
                g.mBrutalSparkEffects.set(on("sparkEffects"));
                g.mNpcBloodDecals.set(on("bloodDecals"));
                g.mKineticPlayerMovement.set(on("kineticMovement"));
                g.mPlayerActiveShieldBlock.set(on("activeBlocking"));
                g.mDarkMessiahSwordParry.set(on("swordParry"));
                g.mDarkMessiahComboAttacks.set(on("comboAttacks"));
                g.mNpcHurtHealthFraction.set(on("npcStagger") ? .001f : 0.f);
                g.mPlayerHitRecovery.set(!on("hitRecovery"));
                g.mBrutalStamina.set(on("stamina"));
                g.mPlayerFatigueRegenFraction.set(on("stamina") ? 1.f/18.f : 0.f);
                g.mBrutalWallBounce.set(on("wallBounce"));
                g.mBrutalControlsEnabled.set(on("controlsEnabled"));
                g.mBrutalSlide.set(on("slide"));
                g.mBrutalClinch.set(on("clinch"));
                g.mBrutalFastAttacks.set(on("fastAttacks"));
                g.mBrutalCombatSounds.set(on("combatSounds"));
                MWBase::World* world = MWBase::Environment::get().getWorld();
                const auto player = MWMechanics::getPlayer();
                const auto mechanics = MWBase::Environment::get().getMechanicsManager();
                auto& stats = player.getClass().getCreatureStats(player);
                if (!g.mBrutalClinch || g.mBrutalClassicFirstPerson || !g.mDarkMessiahSwordParry)
                    mechanics->cancelSwordClinch(player, "settings");
                if (!enabled || oldDual != MWMechanics::brutalDualWield() || oldClassic != g.mBrutalClassicFirstPerson || oldCombo != g.mDarkMessiahComboAttacks || oldFast != g.mBrutalFastAttacks)
                {
                    stats.mPowerStrike = {};
                    stats.mBrutalComboMultiplier = 1.f;
                    stats.mBrutalFastStrike = false;
                    stats.mBrutalOffhandStrike = false;
                }
                if (!enabled || (oldMovement && !g.mKineticPlayerMovement) || (oldSlide && !g.mBrutalSlide))
                    world->clearActorExternalVelocity(player);
                if (!enabled || (!g.mPlayerActiveShieldBlock && !g.mDarkMessiahSwordParry))
                {
                    stats.mBrutalBlockRequested = stats.mBrutalBlockRaised = stats.mBrutalParry = false;
                }
                const auto reload = [world](const MWWorld::Ptr& ptr) {
                    if (auto* animation = dynamic_cast<MWRender::NpcAnimation*>(world->getAnimation(ptr)))
                        animation->reloadBrutalAnimations();
                };
                if (!MWMechanics::brutalDualWield())
                {
                    auto& inventory = player.getClass().getInventoryStore(player);
                    const auto left = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
                    if (left != inventory.end() && left->getType() == ESM::Weapon::sRecordId)
                        inventory.unequipSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
                }
                if (oldDual != MWMechanics::brutalDualWield() || oldClassic != g.mBrutalClassicFirstPerson || oldBody != g.mBrutalFirstPersonBody)
                    reload(player);
                else if (oldBlock != g.mPlayerActiveShieldBlock || oldCombo != g.mDarkMessiahComboAttacks || oldFast != g.mBrutalFastAttacks)
                    mechanics->forceStateUpdate(player);
                if (MWMechanics::brutalDmFirstPerson())
                    if (auto* camera = world->getCamera())
                        camera->setMode(MWRender::Camera::Mode::FirstPerson, true);
                if (oldNpc != g.mBrutalClassicNpcAnimations || oldBlood != g.mNpcBloodDecals)
                {
                    std::vector<MWWorld::Ptr> actors;
                    mechanics->getActorsInRange(player.getRefData().getPosition().asVec3(), 1e7f, actors);
                    for (const auto& ptr : actors) if (ptr != player && ptr.getClass().isNpc()) reload(ptr);
                }
            }, "SetBrutalOption");
        };
        actor["armPowerStrike"] = [context](const SelfObject& self, std::string token,
            sol::optional<Misc::FiniteFloat> multiplier, sol::optional<Misc::FiniteFloat> minimumStrength) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("Power strike requires player SELF");
            const float mult = multiplier ? float(*multiplier) : 4.f;
            const float minimum = minimumStrength ? float(*minimumStrength) : 0.8f;
            if (token.empty() || token.size() > 64 || mult < 1.f || mult > 8.f || minimum < 0.f || minimum > 1.f)
                throw std::runtime_error("Invalid power strike token, multiplier [1,8], or minimum strength [0,1]");
            context.mLuaManager->addAction([object=Object(self), token=std::move(token), mult, minimum] {
                const auto ptr = object.ptrOrEmpty();
                if (ptr.isEmpty() || ptr.getClass().getCreatureStats(ptr).isDead()) return;
                auto& inv = ptr.getClass().getInventoryStore(ptr);
                const auto it = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
                if (it == inv.end() || it->getType() != ESM::Weapon::sRecordId) return;
                const int type = it->get<ESM::Weapon>()->mBase->mData.mType;
                // Finishers work with every melee weapon, one- and two-handed.
                if (MWMechanics::getWeaponType(type)->mWeaponClass != ESM::WeaponType::Melee) return;
                auto& armed = ptr.getClass().getCreatureStats(ptr).mPowerStrike;
                if (ptr.getClass().getCreatureStats(ptr).mLastConsumedPowerStrikeToken != token
                    && (armed.token.empty() || armed.token == token))
                    armed = { token, *it, mult, minimum };
            }, "ArmPowerStrike");
        };
        actor["cancelPowerStrike"] = [context](const SelfObject& self, std::string token) {
            if (self.ptr() != MWMechanics::getPlayer()) throw std::runtime_error("Power strike requires player SELF");
            context.mLuaManager->addAction([object=Object(self), token=std::move(token)] {
                const auto ptr = object.ptrOrEmpty();
                if (ptr.isEmpty()) return;
                auto& armed = ptr.getClass().getCreatureStats(ptr).mPowerStrike;
                if (armed.token == token) armed = {};
            }, "CancelPowerStrike");
        };
        actor["getPowerStrikeToken"] = [](const SelfObject& self) {
            if (self.ptr() != MWMechanics::getPlayer()) throw std::runtime_error("Power strike requires player SELF");
            const auto& armed = self.ptr().getClass().getCreatureStats(self.ptr()).mPowerStrike;
            return armed.token;
        };

        actor["stagger"] = [context](const SelfObject& self) {
            const auto initial = self.ptr();
            if (!initial.getClass().isActor() || initial == MWMechanics::getPlayer())
                throw std::runtime_error("Stagger requires a non-player actor SELF");
            if (!initial.isInCell() || !initial.getRefData().isEnabled()
                || initial.getClass().getCreatureStats(initial).isDead())
                return false;
            context.mLuaManager->addAction([object = Object(self)] {
                const auto ptr = object.ptrOrEmpty();
                if (ptr.isEmpty() || !ptr.isInCell() || !ptr.getRefData().isEnabled()) return;
                MWBase::Environment::get().getMechanicsManager()->staggerActor(ptr);
            }, "StaggerNpc");
            return true; // Accepted for the main-thread controller; death/unload is rechecked there.
        };

        actor["configureSwordClinch"] = [context](const SelfObject& self, sol::table options) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("configureSwordClinch requires player SELF");
            const bool enabled = options["enabled"].get_or(false);
            const int required = options["requiredClicks"].get_or(3);
            const float duration = options["duration"].get_or(1.4333333f);
            const float distance = options["distance"].get_or(81.07684f);
            if (required < 1 || required > 30 || !std::isfinite(duration) || duration < .5f || duration > 5.f
                || !std::isfinite(distance) || distance < 55.f || distance > 125.f)
                throw std::runtime_error("Invalid clinch clicks [1,30], duration [.5,5], or distance [55,125]");
            context.mLuaManager->addAction([object = Object(self), enabled, required, duration, distance] {
                const auto ptr = object.ptrOrEmpty();
                if (ptr.isEmpty()) return;
                auto& stats = ptr.getClass().getCreatureStats(ptr);
                stats.mBrutalClinchEnabled = enabled;
                stats.mBrutalClinchRequiredClicks = required;
                stats.mBrutalClinchDuration = duration;
                stats.mBrutalClinchDistance = distance;
                if (!enabled)
                    MWBase::Environment::get().getMechanicsManager()->cancelSwordClinch(ptr, "disabled");
            }, "ConfigureSwordClinch");
        };
        actor["getSwordClinchState"] = [lua = context.mLua](const Object& object) {
            const auto& ptr = object.ptr();
            const auto& stats = ptr.getClass().getCreatureStats(ptr);
            sol::table result(lua->unsafeState(), sol::create);
            result["active"] = stats.mBrutalClinchActive;
            result["pending"] = !stats.mBrutalClinchActive && !stats.mBrutalClinchOpponent.isEmpty();
            result["id"] = stats.mBrutalClinchId;
            static constexpr std::array<std::string_view, 5> phases{ "idle", "align", "contest", "win", "lose" };
            result["phase"] = phases.at(stats.mBrutalClinchPhase);
            result["clicks"] = stats.mBrutalClinchClicks;
            result["requiredClicks"] = stats.mBrutalClinchTarget;
            result["progress"] = stats.mBrutalClinchTarget > 0
                ? std::clamp(float(stats.mBrutalClinchClicks) / stats.mBrutalClinchTarget, 0.f, 1.f) : 0.f;
            result["remaining"] = stats.mBrutalClinchRemaining;
            result["elapsed"] = stats.mBrutalClinchElapsed;
            result["group"] = stats.mBrutalClinchPhase == 3 ? "brutal_clinch_win"
                : stats.mBrutalClinchPhase == 4 ? "brutal_clinch_lose"
                : stats.mBrutalClinchActive ? "brutal_clinch" : "";
            if (!stats.mBrutalClinchOpponent.isEmpty()) result["opponent"] = LObject(stats.mBrutalClinchOpponent);
            result["lastResultId"] = stats.mBrutalClinchLastResultId;
            result["lastResult"] = stats.mBrutalClinchLastResult;
            result["lastReason"] = stats.mBrutalClinchLastReason;
            return result;
        };
        actor["_getSwordClinchGeometryForAudit"] = [lua = context.mLua](const SelfObject& self) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("_getSwordClinchGeometryForAudit requires player SELF");
            sol::table result(lua->unsafeState(), sol::create);
            const auto* animation = dynamic_cast<const MWRender::NpcAnimation*>(
                MWBase::Environment::get().getWorld()->getAnimation(self.ptr()));
            if (!animation)
                return result;
            const auto& a = animation->getSwordClinchGeometryForAudit();
            result["active"] = a.active;
            result["fitted"] = a.fitted;
            result["clamped"] = a.clamped;
            result["frame"] = a.frame;
            result["phase"] = a.phase;
            result["elapsed"] = a.elapsed;
            result["sourceElapsed"] = a.sourceElapsed;
            result["weight"] = a.weight;
            result["rawGap"] = a.rawGap;
            result["finalGap"] = a.finalGap;
            result["cameraError"] = a.cameraError;
            result["correction"] = a.correction;
            result["requestedCorrection"] = a.requestedCorrection;
            result["playerHilt"] = a.playerHilt;
            result["playerAxis"] = a.playerAxis;
            result["opponentHilt"] = a.opponentHilt;
            result["opponentAxis"] = a.opponentAxis;
            result["playerRange"] = a.playerRange;
            result["opponentRange"] = a.opponentRange;
            result["playerContact"] = a.playerContact;
            result["opponentContact"] = a.opponentContact;
            result["playerParameter"] = a.playerParameter;
            result["opponentParameter"] = a.opponentParameter;
            return result;
        };
        actor["cancelSwordClinch"] = [context](const SelfObject& self) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("cancelSwordClinch requires player SELF");
            context.mLuaManager->addAction([object = Object(self)] {
                const auto ptr = object.ptrOrEmpty();
                if (!ptr.isEmpty())
                    MWBase::Environment::get().getMechanicsManager()->cancelSwordClinch(ptr, "requested");
            }, "CancelSwordClinch");
        };

        actor["setShieldBlock"] = [context](const SelfObject& self, bool held) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("setShieldBlock requires player SELF");
            context.mLuaManager->addAction([object = Object(self), held] {
                const auto ptr = object.ptrOrEmpty();
                if (!ptr.isEmpty())
                    ptr.getClass().getCreatureStats(ptr).mBrutalBlockRequested = held;
            }, "ShieldBlockRequest");
        };
        // Dark Messiah gait for the kinetic player: 1 walk, 2 run (default), 3 sprint;
        // 0 restores Morrowind's run stance. Stamina rules stay in the mod.
        actor["setKineticGait"] = [context](const SelfObject& self, int gait) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("setKineticGait requires player SELF");
            if (gait < 0 || gait > 3)
                throw std::runtime_error("setKineticGait: gait must be 0..3");
            context.mLuaManager->addAction([object = Object(self), gait] {
                const auto ptr = object.ptrOrEmpty();
                if (!ptr.isEmpty())
                    ptr.getClass().getCreatureStats(ptr).mBrutalGait = gait;
            }, "KineticGait");
        };
        actor["getKineticGait"] = [](const Object& object) {
            const auto& ptr = object.ptr();
            return ptr.getClass().isActor() ? ptr.getClass().getCreatureStats(ptr).mBrutalGait : 0;
        };
        actor["requestKineticSlide"] = [context](const SelfObject& self) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("requestKineticSlide requires player SELF");
            context.mLuaManager->addAction([object = Object(self)] {
                const auto ptr = object.ptrOrEmpty();
                if (!ptr.isEmpty())
                    ptr.getClass().getCreatureStats(ptr).mBrutalSlideRequested = true;
            }, "KineticSlide");
        };
        actor["tripKineticSlideNpc"] = [context](const SelfObject& self, const Object& target,
                                               Misc::FiniteVec3f value) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("tripKineticSlideNpc requires player SELF");
            const osg::Vec3f kick = value;
            if (kick.length2() < 1.f || kick.length2() > 1000.f * 1000.f)
                throw std::runtime_error("Slide trip velocity must be 1..1000 units/s");
            // Queue directly from SELF: an additional global-script relay can
            // leave the standing hull in the way for another low-FPS frame.
            context.mLuaManager->addAction([sourceObject = Object(self), target, kick] {
                const auto source = sourceObject.ptrOrEmpty(), victim = target.ptrOrEmpty();
                const auto world = MWBase::Environment::get().getWorld();
                if (source.isEmpty() || victim.isEmpty() || source != world->getPlayerPtr()
                    || !source.isInCell() || !victim.isInCell() || source.getCell() != victim.getCell()
                    || source == victim || !victim.getClass().isActor()
                    || !victim.getRefData().isEnabled() || !victim.getRefData().getBaseNode())
                    return;
                const auto& stats = source.getClass().getCreatureStats(source);
                if (stats.isDead() || stats.isParalyzed() || stats.mBrutalSlidePhase == 0
                    || !stats.mBrutalSlideLow || victim.getClass().getCreatureStats(victim).isDead())
                    return;
                const auto sourcePosition = source.getRefData().getPosition().asVec3();
                const auto targetPosition = victim.getRefData().getPosition().asVec3();
                if ((targetPosition - sourcePosition).length2() > 170.f * 170.f)
                    return;
                const auto from = sourcePosition + osg::Vec3f(0, 0, 20);
                const auto to = targetPosition + osg::Vec3f(0, 0, 20);
                constexpr int coverMask = MWPhysics::CollisionType_World | MWPhysics::CollisionType_Door
                    | MWPhysics::CollisionType_HeightMap | MWPhysics::CollisionType_Dynamic;
                const auto cover = world->getRayCasting()->castRay(from, to, {source, victim}, {}, coverMask);
                if (cover.mHit && (cover.mHitPos - from).length() + .05f < (to - from).length())
                    return;
                // This handoff neither deals damage nor consumes a sword/kick
                // charge. The existing knockdown owns physics and equipment.
                world->applyNpcKick(victim, kick, source, true, true);
            }, "KineticSlideTrip");
            return true; // Request queued; the action revalidates current contact conditions.
        };
        actor["getKineticSlide"] = [context](const Object& object) {
            const auto& stats = object.ptr().getClass().getCreatureStats(object.ptr());
            sol::table result(context.mLua->unsafeState(), sol::create);
            result["phase"] = stats.mBrutalSlidePhase;
            result["moving"] = stats.mBrutalSliding;
            result["low"] = stats.mBrutalSlideLow || stats.mBrutalSlideBlocked;
            result["blocked"] = stats.mBrutalSlideBlocked;
            result["blend"] = stats.mBrutalSlideBlend;
            result["attacking"] = stats.mBrutalSlideAttack;
            result["downhill"] = stats.mBrutalSlideDownhill;
            result["groundNormal"] = stats.mBrutalSlideGroundNormal;
            result["groundPoint"] = stats.mBrutalSlideGroundPoint;
            return result;
        };
        // Read-only diagnosis of perceived player height against ordinary actors.
        actor["isHeadSevered"] = [](const Object& object) {
            const auto& ptr=object.ptr();
            if(!ptr.getClass().isNpc())return false;
            const auto* animation=dynamic_cast<const MWRender::NpcAnimation*>(
                MWBase::Environment::get().getWorld()->getAnimation(ptr));
            return animation&&animation->getDecapitationStats().at("severed")!=0;
        };
        actor["getTorsoSeveredPoseData"] = [](const Object& object) {
            const auto& ptr = object.ptr();
            return ptr.getClass().isNpc()
                ? MWBase::Environment::get().getWorld()->getNpcBisectionPoseData(ptr) : std::string();
        };
        actor["getSeveredLimbsMask"] = [](const Object& object) {
            return MWBase::Environment::get().getWorld()->getNpcSeveredLimbsMask(object.ptr());
        };
        actor["getLimbSeveringPoseData"] = [](const Object& object) {
            return MWBase::Environment::get().getWorld()->getNpcLimbSeveringPoseData(object.ptr());
        };
        actor["isTorsoSevered"] = [](const Object& object) {
            const auto& ptr = object.ptr();
            if (!ptr.getClass().isNpc()) return false;
            const auto* animation = dynamic_cast<const MWRender::NpcAnimation*>(
                MWBase::Environment::get().getWorld()->getAnimation(ptr));
            return animation && animation->isTorsoSevered();
        };
        actor["_getHeightStateForAudit"] = [lua = context.mLua](const Object& object) {
            const auto& ptr = object.ptr();
            if (!ptr.getClass().isActor())
                throw std::runtime_error("_getHeightStateForAudit requires an actor");
            auto world = MWBase::Environment::get().getWorld();
            sol::table state(lua->unsafeState(), sol::create);
            state["position"] = ptr.getRefData().getPosition().asVec3();
            state["referenceScale"] = ptr.getCellRef().getScale();
            osg::Vec3f scale(ptr.getCellRef().getScale(), ptr.getCellRef().getScale(), ptr.getCellRef().getScale());
            ptr.getClass().adjustScale(ptr, scale, true);
            state["renderScale"] = scale;
            state["collisionHalfExtents"] = world->getHalfExtents(ptr);
            state["visualHalfExtents"] = world->getHalfExtents(ptr, true);
            const auto* animation = world->getAnimation(ptr);
            state["hasAnimation"] = animation != nullptr;
            if (!animation)
                return state;
            for (const auto& [field, bone] : std::array<std::pair<const char*, const char*>, 16>{ {
                { "head", "Bip01 Head" }, { "neck", "Bip01 Neck" }, { "pelvis", "Bip01 Pelvis" },
                { "waist", "Bip01 Spine1" },
                { "leftFoot", "Bip01 L Foot" }, { "rightFoot", "Bip01 R Foot" },
                { "leftKnee", "Bip01 L Calf" }, { "rightKnee", "Bip01 R Calf" },
                { "leftHip", "Bip01 L Thigh" }, { "rightHip", "Bip01 R Thigh" },
                { "leftShoulder", "Bip01 L UpperArm" }, { "rightShoulder", "Bip01 R UpperArm" },
                { "leftElbow", "Bip01 L Forearm" }, { "rightElbow", "Bip01 R Forearm" },
                { "leftHand", "Bip01 L Hand" }, { "rightHand", "Bip01 R Hand" } } })
                if (const auto matrix = animation->getBoneWorldMatrix(bone))
                    state[field] = osg::Vec3f(matrix->getTrans());
            if (const auto head = animation->getBoneWorldMatrix("Bip01 Head"))
            {
                // Read actual rendered bone orientation, not the latest physics target.
                state["headRight"] = osg::Matrixf::transform3x3(osg::Vec3f(1,0,0), *head);
                state["headForward"] = osg::Matrixf::transform3x3(osg::Vec3f(0,1,0), *head);
                state["headUp"] = osg::Matrixf::transform3x3(osg::Vec3f(0,0,1), *head);
            }
            if (const auto* npc = dynamic_cast<const MWRender::NpcAnimation*>(animation))
            {
                sol::table shield(lua->unsafeState(), sol::create);
                for (const auto& [name, value] : npc->getShieldGeometryForAudit()) shield[name] = value;
                state["shieldGeometry"] = std::move(shield);
                if (const auto* body = npc->getBodyAwarenessAnimation())
                    if (const auto matrix = body->getBoneWorldMatrix("Bip01 Head"))
                        state["bodyHead"] = osg::Vec3f(matrix->getTrans());
            }
            return state;
        };

        // Explicit, one-shot surface audit. Large triangle payloads are kept
        // separate from ordinary per-frame controller observations.
        actor["_getWeaponGripGeometryForAudit"] = [lua = context.mLua](const SelfObject& self) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("_getWeaponGripGeometryForAudit requires player SELF");
            auto* animation = dynamic_cast<MWRender::NpcAnimation*>(
                MWBase::Environment::get().getWorld()->getAnimation(self.ptr()));
            sol::table result(lua->unsafeState(), sol::create);
            if (!animation)
                return result;
            const auto snapshot = animation->getWeaponGripGeometryForAudit();
            sol::table models(lua->unsafeState(), sol::create), roots(lua->unsafeState(), sol::create),
                triangles(lua->unsafeState(), sol::create);
            for (const auto& [name, model] : snapshot.models)
                models[name] = model;
            for (const auto& [name, matrix] : snapshot.roots)
            {
                sol::table pose(lua->unsafeState(), sol::create);
                pose["position"] = osg::Vec3f(matrix.getTrans());
                pose["x"] = osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), matrix);
                pose["y"] = osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), matrix);
                pose["z"] = osg::Matrixf::transform3x3(osg::Vec3f(0, 0, 1), matrix);
                roots[name] = std::move(pose);
            }
            for (const auto& [name, points] : snapshot.triangles)
            {
                sol::table vertices(lua->unsafeState(), sol::create);
                std::size_t index = 1;
                for (const auto& point : points)
                    vertices[index++] = point;
                triangles[name] = std::move(vertices);
            }
            result["models"] = std::move(models);
            result["roots"] = std::move(roots);
            result["triangles"] = std::move(triangles);
            return result;
        };

        // Audit-only: observe the rendered body, weapon bones and controller state.
        actor["_getFirstPersonBodyStateForAudit"] = [lua = context.mLua](const SelfObject& self) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("_getFirstPersonBodyStateForAudit requires player SELF");
            sol::table state(lua->unsafeState(), sol::create);
            auto* animation = dynamic_cast<MWRender::NpcAnimation*>(
                MWBase::Environment::get().getWorld()->getAnimation(self.ptr()));
            const auto* body = animation ? animation->getBodyAwarenessAnimation() : nullptr;
            state["present"] = body != nullptr;
            if (!animation)
                return state;
            state["armsMask"] = animation->getObjectRoot()->getNodeMask();
            {
                sol::table bones(lua->unsafeState(), sol::create);
                for (const auto& [name, bone] : { std::pair{ "rightHand", "Bip01 R Hand" },
                         std::pair{ "leftHand", "Bip01 L Hand" }, std::pair{ "weapon", "Weapon Bone" },
                         std::pair{ "leftWeapon", "Brutal Left Weapon" },
                         std::pair{ "rightDaggerFrame", "Brutal Right Dagger Frame" },
                         std::pair{ "leftDaggerFrame", "Brutal Left Dagger Frame" },
                         std::pair{ "rightForearm", "Bip01 R Forearm" },
                         std::pair{ "leftForearm", "Bip01 L Forearm" },
                         std::pair{ "rightUpperArm", "Bip01 R UpperArm" },
                         std::pair{ "leftUpperArm", "Bip01 L UpperArm" } })
                    if (const auto matrix = animation->getBoneWorldMatrix(bone))
                    {
                        sol::table pose(lua->unsafeState(), sol::create);
                        pose["position"] = osg::Vec3f(matrix->getTrans());
                        pose["x"] = osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), *matrix);
                        pose["y"] = osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), *matrix);
                        pose["z"] = osg::Matrixf::transform3x3(osg::Vec3f(0, 0, 1), *matrix);
                        bones[name] = std::move(pose);
                    }
                for (int finger = 0; finger < 5; ++finger)
                    for (int joint = 0; joint < 3; ++joint)
                    {
                        const std::string suffix = std::to_string(finger)
                            + (joint ? std::to_string(joint) : "");
                        if (const auto matrix = animation->getBoneWorldMatrix("Bip01 L Finger" + suffix))
                        {
                            sol::table pose(lua->unsafeState(), sol::create);
                            pose["position"] = osg::Vec3f(matrix->getTrans());
                            pose["x"] = osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), *matrix);
                            pose["y"] = osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), *matrix);
                            pose["z"] = osg::Matrixf::transform3x3(osg::Vec3f(0, 0, 1), *matrix);
                            bones["leftFinger" + suffix] = std::move(pose);
                        }
                    }
                state["weaponCycleBones"] = std::move(bones);
                const auto& audit = animation->getWeaponCycleForAudit();
                sol::table cycle(lua->unsafeState(), sol::create);
                cycle["phase"] = std::string(audit.phase);
                cycle["index"] = audit.index;
                cycle["upperBody"] = audit.upperBody;
                cycle["queued"] = audit.queued;
                cycle["button"] = audit.button;
                cycle["readyToHit"] = audit.readyToHit;
                cycle["held"] = audit.held;
                cycle["chargedHold"] = audit.chargedHold;
                cycle["guardCharge"] = audit.guardCharge;
                cycle["useHeld"] = audit.useHeld;
                cycle["acceptedStrikes"] = audit.acceptedStrikes;
                state["weaponCycleState"] = std::move(cycle);
            }
            {
                const auto& a = animation->getSwordWallContactForAudit();
                sol::table wall(lua->unsafeState(), sol::create);
                wall["count"] = a.count;
                wall["soundPlayed"] = a.soundPlayed;
                wall["sounds"] = a.sounds;
                wall["armed"] = a.armed;
                wall["samples"] = a.samples;
                wall["frame"] = a.frame;
                wall["sourceTime"] = a.sourceTime;
                wall["sweepSegments"] = a.sweepSegments;
                wall["mount"] = a.mount;
                wall["sweepCandidates"] = a.sweepCandidates;
                wall["sweepConfirmed"] = a.sweepConfirmed;
                wall["sweepMilliseconds"] = a.sweepMilliseconds;
                wall["key"] = a.key;
                wall["kind"] = a.kind;
                wall["position"] = a.position;
                wall["normal"] = a.normal;
                wall["previousBase"] = a.previousBase;
                wall["previousTip"] = a.previousTip;
                wall["bladeBase"] = a.bladeBase;
                wall["bladeTip"] = a.bladeTip;
                wall["sweepFrom"] = a.sweepFrom;
                wall["sweepTo"] = a.sweepTo;
                wall["sweepFraction"] = a.sweepFraction;
                wall["radius"] = 0.f;
                wall["currentBase"] = a.currentBase;
                wall["currentTip"] = a.currentTip;
                wall["currentContactPoint"] = a.currentContactPoint;
                state["wallBounce"] = std::move(wall);
            }
            state["bodyMask"] = animation->getBodyAwarenessVisibilityMask();
            state["bodyHiddenForClinch"] = animation->isBodyAwarenessHiddenForClinch();
            state["primaryCarriedLeftShown"] = animation->getCarriedLeftShown();
            if (const auto matrix = animation->getBoneWorldMatrix("Camera"))
                state["primaryCameraPosition"] = osg::Vec3f(matrix->getTrans());
            if (const auto matrix = animation->getBoneWorldMatrix("Brutal Camera Pivot"))
                state["primaryPivotPosition"] = osg::Vec3f(matrix->getTrans());
            if (const auto matrix = animation->getBoneWorldMatrix("Brutal Camera Carrier"))
                state["primaryCarrierPosition"] = osg::Vec3f(matrix->getTrans());
            if (const auto matrix = animation->getBoneWorldMatrix("Shield Bone"))
            {
                state["primaryShieldPosition"] = osg::Vec3f(matrix->getTrans());
                state["primaryShieldX"] = osg::Vec3f((*matrix)(0, 0), (*matrix)(0, 1), (*matrix)(0, 2));
                state["primaryShieldY"] = osg::Vec3f((*matrix)(1, 0), (*matrix)(1, 1), (*matrix)(1, 2));
                state["primaryShieldZ"] = osg::Vec3f((*matrix)(2, 0), (*matrix)(2, 1), (*matrix)(2, 2));
                state["primaryShieldScale"] = osg::Vec3f(matrix->getScale());
            }
            if (const auto matrix = animation->getBoneWorldMatrix("Weapon Bone"))
                state["primaryWeaponPosition"] = osg::Vec3f(matrix->getTrans());
            const auto primaryGroup = animation->getActiveGroup(MWRender::BoneGroup_LowerBody);
            state["primaryLowerGroup"] = std::string(primaryGroup);
            state["primaryTime"] = animation->getCurrentTime(primaryGroup);
            float completion = 0.f;
            animation->getInfo(primaryGroup, &completion);
            state["primaryCompletion"] = completion;
            if (body)
            {
                state["bodyGroundUp"] = animation->getBodyGroundUp();
                state["bodyGroundAnchor"] = animation->getBodyGroundAnchor();
                state["bodyGroundBlend"] = animation->getBodyGroundBlend();
                state["bodyGroundLift"] = animation->getBodyGroundLift();
                const auto bodyGroup = body->getActiveGroup(MWRender::BoneGroup_LowerBody);
                state["bodyLowerGroup"] = std::string(bodyGroup);
                state["bodyTime"] = body->getCurrentTime(bodyGroup);
                completion = 0.f;
                body->getInfo(bodyGroup, &completion);
                state["bodyCompletion"] = completion;
                if (const auto foot = body->getBoneWorldMatrix("Bip01 L Foot"))
                    state["leftFoot"] = osg::Vec3f(foot->getTrans());
                if (const auto foot = body->getBoneWorldMatrix("Bip01 R Foot"))
                    state["rightFoot"] = osg::Vec3f(foot->getTrans());
                if (const auto pelvis = body->getBoneWorldMatrix("Bip01 Pelvis"))
                    state["bodyPelvisPosition"] = osg::Vec3f(pelvis->getTrans());
                if (const auto spine = body->getBoneWorldMatrix("Bip01 Spine2"))
                    state["bodySpinePosition"] = osg::Vec3f(spine->getTrans());
                for (const auto& [key, bone] : { std::pair{ "leftHip", "Bip01 L Thigh" },
                         std::pair{ "rightHip", "Bip01 R Thigh" }, std::pair{ "leftKnee", "Bip01 L Calf" },
                         std::pair{ "rightKnee", "Bip01 R Calf" } })
                    if (const auto matrix = body->getBoneWorldMatrix(bone))
                        state[key] = osg::Vec3f(matrix->getTrans());
                osg::Vec3f normal = self.ptr().getClass().getCreatureStats(self.ptr()).mBrutalSlideGroundNormal;
                if (normal.length2() < .9f)
                    normal = animation->getBodyGroundUp();
                for (const auto& [key, value] : body->getFootGeometryForAudit(normal))
                    state[key] = value;
            }
            return state;
        };
        actor["_setBodyVisibleForAudit"] = [context](const SelfObject& self, bool visible) {
            if (self.ptr() != MWMechanics::getPlayer())
                throw std::runtime_error("_setBodyVisibleForAudit requires player SELF");
            context.mLuaManager->addAction([object = Object(self), visible] {
                const auto ptr = object.ptrOrEmpty();
                if (ptr.isEmpty()) return;
                if (auto* animation = dynamic_cast<MWRender::NpcAnimation*>(
                        MWBase::Environment::get().getWorld()->getAnimation(ptr)))
                    animation->setFirstPersonGeometryVisibleForAudit(visible);
            }, "AuditBodyVisibility");
        };
        actor["isShieldBlockRaised"] = [](const Object& object) {
            const auto& ptr = object.ptr();
            return ptr.getClass().isActor() && ptr.getClass().getCreatureStats(ptr).mBrutalBlockRaised;
        };

        actor["addExternalVelocity"] = [context](const SelfObject& self, Misc::FiniteVec3f value) {
            const osg::Vec3f delta = value;
            if (!self.ptr().getClass().isActor())
                throw std::runtime_error("Actor expected");
            if (delta.length2() > 2000.f * 2000.f)
                throw std::runtime_error("External velocity delta exceeds 2000 game units/s");
            context.mLuaManager->addAction([object = Object(self), delta] {
                const MWWorld::Ptr ptr = object.ptrOrEmpty();
                if (ptr.isEmpty() || !ptr.isInCell() || ptr.getClass().getCreatureStats(ptr).isDead())
                    return;
                MWBase::Environment::get().getWorld()->addActorExternalVelocity(ptr, delta);
            }, "AddExternalVelocity");
        };

        actor["_onHit"] = [context](const SelfObject& self, const sol::table& options) {
            std::optional<osg::Vec3f> lethalVelocity;
            if (const auto value = options.get<sol::optional<Misc::FiniteVec3f>>("brutalRagdollVelocity"))
            {
                const osg::Vec3f velocity = *value;
                if (velocity.length2() > 2000.f * 2000.f)
                    throw std::runtime_error("Lethal ragdoll velocity exceeds 2000 game units/s");
                lethalVelocity = velocity;
            }
            std::optional<osg::Vec3f> lethalPoint;
            if (const auto value = options.get<sol::optional<Misc::FiniteVec3f>>("hitPos"))
                lethalPoint = osg::Vec3f(*value);
            sol::optional<sol::table> damageLua = options.get<sol::optional<sol::table>>("damage");
            std::map<std::string, float> damageCpp;
            if (damageLua)
            {
                for (auto& [key, value] : damageLua.value())
                {
                    damageCpp[key.as<std::string>()] = value.as<Misc::FiniteFloat>();
                }
            }
            std::string sourceTypeStr = options.get_or<std::string>("sourceType", "unspecified");
            MWMechanics::DamageSourceType sourceType = MWMechanics::DamageSourceType::Unspecified;
            if (sourceTypeStr == "melee")
                sourceType = MWMechanics::DamageSourceType::Melee;
            else if (sourceTypeStr == "ranged")
                sourceType = MWMechanics::DamageSourceType::Ranged;
            else if (sourceTypeStr == "magic")
                sourceType = MWMechanics::DamageSourceType::Magical;
            std::string_view ammoId = options.get_or<std::string_view>("ammo", {});
            ESM::RefId weaponId = ESM::RefId::deserializeText(ammoId);
            if (weaponId.empty())
            {
                if (sol::optional<Object> weapon = options.get<sol::optional<Object>>("weapon"))
                {
                    MWWorld::Ptr weaponPtr = weapon->ptrOrEmpty();
                    if (!weaponPtr.isEmpty())
                        weaponId = weaponPtr.getCellRef().getRefId();
                }
            }

            context.mLuaManager->addAction(
                [self = Object(self), damages = std::move(damageCpp),
                    attacker = options.get<sol::optional<Object>>("attacker"), weaponId,
                    successful = options.get<bool>("successful"), sourceType = sourceType, lethalVelocity, lethalPoint,
                    attackId = options.get_or<uint64_t>("brutalAttackId", 0),
                    lua = context.mLua, manager = context.mLuaManager] {
                    MWWorld::Ptr attackerPtr;
                    MWWorld::Ptr weaponPtr;
                    if (attacker)
                        attackerPtr = attacker->ptrOrEmpty();
                    const auto target = self.ptrOrEmpty();
                    if (target.isEmpty()) return;
                    const auto& stats = target.getClass().getCreatureStats(target);
                    const bool aliveBefore = !stats.isDead();
                    const float healthBefore = stats.getHealth().getCurrent();
                    target.getClass().onHit(target, damages, weaponId, attackerPtr, successful, sourceType);
                    if (lethalVelocity && aliveBefore && stats.isDead()
                        && healthBefore > stats.getHealth().getCurrent()
                        && (target.getType() == ESM::REC_NPC_ || MWPhysics::findCreatureRig(target))
                        && target != MWMechanics::getPlayer())
                    {
                        // Capture the last rendered lethal-hit pose before Mechanics can
                        // select/evaluate any canned death animation. A living
                        // knockdown retains its body and receives this hit once too.
                        try
                        {
                            auto world = MWBase::Environment::get().getWorld();
                            if (Settings::game().mRagdollEnhancement && lethalPoint)
                            {
                                // The hit owns one primary point impulse. Starting
                                // with zero kick avoids stacking the old launch.
                                const bool started = world->startNpcRagdoll(target, osg::Vec3f());
                                // A native attack ID deduplicates the impulse on an
                                // existing living ragdoll without restarting it.
                                if (started || (attackId != 0 && !world->getNpcRagdollState(target).empty()))
                                    world->applyNpcRagdollImpact(target, std::numeric_limits<std::size_t>::max(),
                                        *lethalPoint, *lethalVelocity * 15.f, attackId);
                            }
                            else world->startNpcRagdoll(target, *lethalVelocity);
                        }
                        catch (const std::exception& error)
                        {
                            Log(Debug::Warning) << "Immediate lethal ragdoll unavailable: " << error.what();
                        }
                    }
                    if (attackId != 0)
                        lua->protectedCall([&](LuaUtil::LuaView& view) {
                            sol::table receipt = view.newTable();
                            receipt["attackId"] = attackId;
                            receipt["aliveBefore"] = aliveBefore;
                            receipt["deadAfter"] = stats.isDead();
                            receipt["actualHealthDamage"] = std::max(0.f, healthBefore-stats.getHealth().getCurrent());
                            manager->sendLocalEvent(target, "BrutalHitApplied", receipt);
                        });
                },
                "HitAction");
        };

        addActorStatsBindings(actor, context);
        addActorMagicBindings(actor, context);
    }
}
