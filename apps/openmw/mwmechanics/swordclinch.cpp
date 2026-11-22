#include "../mwphysics/collisiontype.hpp"
#include "actors.hpp"

#include <algorithm>
#include <cmath>

#include <components/esm3/loadweap.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwphysics/raycasting.hpp"
#include "../mwrender/animation.hpp"
#include "../mwrender/npcanimation.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/inventorystore.hpp"

#include "actorutil.hpp"
#include "brutaloptions.hpp"
#include "character.hpp"
#include "creaturestats.hpp"
#include "damagesourcetype.hpp"
#include "movement.hpp"

namespace MWMechanics
{
    namespace
    {
        MWWorld::Ptr clinchSword(const MWWorld::Ptr& actor, bool player)
        {
            if (actor.isEmpty() || !actor.getClass().isNpc())
                return {};
            auto& inventory = actor.getClass().getInventoryStore(actor);
            const auto right = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
            if (right == inventory.end() || right->getType() != ESM::Weapon::sRecordId)
                return {};
            const int type = right->get<ESM::Weapon>()->mBase->mData.mType;
            // The player's dagger and staff sets carry their own contest clips; an NPC opponent
            // still needs the sword it has clips for.
            const bool playerSet = player && (type == ESM::Weapon::SpearTwoWide || type == ESM::Weapon::BluntTwoWide);
            if (!playerSet && type != ESM::Weapon::ShortBladeOneHand && type != ESM::Weapon::LongBladeOneHand
                && (player || type != ESM::Weapon::LongBladeTwoHand))
                return {};
            if (player)
            {
                const auto left = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
                if (left != inventory.end() && left->getType() == ESM::Armor::sRecordId)
                    return {};
            }
            return *right;
        }

        constexpr int terrainMask
            = MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap | MWPhysics::CollisionType_Door;

        bool clinchGround(const osg::Vec3f& position)
        {
            const auto* rays = MWBase::Environment::get().getWorld()->getRayCasting();
            // Check the actual support beneath both feet, including a ledge at
            // either side. The ordinary physics hull still decides all movement.
            for (const osg::Vec3f offset : { osg::Vec3f(), osg::Vec3f(12, 0, 0), osg::Vec3f(-12, 0, 0),
                     osg::Vec3f(0, 12, 0), osg::Vec3f(0, -12, 0) })
            {
                const auto hit = rays->castRay(
                    position + offset + osg::Vec3f(0, 0, 16), position + offset - osg::Vec3f(0, 0, 24), terrainMask);
                if (!hit.mHit || hit.mHitNormal.z() < .85f)
                    return false;
            }
            return true;
        }

        float clipLength(const MWWorld::Ptr& actor, std::string_view group)
        {
            const auto* animation = MWBase::Environment::get().getWorld()->getAnimation(actor);
            if (!animation)
                return 0.f;
            const std::string name(group);
            return std::max(
                0.f, animation->getTextKeyTime(name + ": stop") - animation->getTextKeyTime(name + ": start"));
        }
    }

    bool Actors::requestSwordClinch(const MWWorld::Ptr& player, const MWWorld::Ptr& opponent)
    {
        if (player != getPlayer() || opponent == player || opponent.isEmpty() || mSwordClinch)
            return false;
        const auto& playerStats = player.getClass().getCreatureStats(player);
        if (!Settings::game().mBrutalClinch || !playerStats.mBrutalClinchEnabled || !Settings::game().mDarkMessiahSwordParry || !brutalDmFirstPerson())
            return false;
        const auto playerIt = mIndex.find(player.mRef), opponentIt = mIndex.find(opponent.mRef);
        if (playerIt == mIndex.end() || opponentIt == mIndex.end()
            || !playerIt->second->getCharacterController().canSwordClinch()
            || !opponentIt->second->getCharacterController().canSwordClinch())
            return false;
        const auto playerWeapon = clinchSword(player, true), opponentWeapon = clinchSword(opponent, false);
        if (playerWeapon.isEmpty() || opponentWeapon.isEmpty())
            return false;
        auto world = MWBase::Environment::get().getWorld();
        auto* playerAnimation = dynamic_cast<MWRender::NpcAnimation*>(world->getAnimation(player));
        auto* opponentAnimation = dynamic_cast<MWRender::NpcAnimation*>(world->getAnimation(opponent));
        if (!playerAnimation || !opponentAnimation || !playerAnimation->canFitSwordClinchPose(*opponentAnimation))
            return false;
        SwordClinch session;
        session.player = player;
        session.opponent = opponent;
        session.playerWeapon = playerWeapon;
        session.opponentWeapon = opponentWeapon;
        session.playerOrigin = player.getRefData().getPosition().asVec3();
        session.direction = opponent.getRefData().getPosition().asVec3() - session.playerOrigin;
        session.direction.z() = 0.f;
        const float separation = session.direction.normalize();
        if (separation < 35.f || separation > 190.f)
            return false;
        // Dark Messiah's Weapon_Vs_Weapon keeps the player's view: the opponent comes
        // to the player's forward line. The placement itself happens at the first actor update.
        const float playerYaw = player.getRefData().getPosition().rot[2];
        session.forward = osg::Vec3f(std::sin(playerYaw), std::cos(playerYaw), 0.f);
        session.distance = playerStats.mBrutalClinchDistance;
        session.opponentGoal = session.playerOrigin + session.direction * session.distance;
        session.requiredClicks = playerStats.mBrutalClinchRequiredClicks;
        session.duration = playerStats.mBrutalClinchDuration;
        session.waitingRelease = mSwordClinchUseHeld;
        mSwordClinch = session;
        if (!validSwordClinch(false) || !world->isOnGround(player) || !world->isOnGround(opponent))
        {
            mSwordClinch.reset();
            return false;
        }
        // Claim both actors at the contact callback. Ordinary combat cannot steal
        // the pending pair before its authored animation starts next update.
        player.getClass().getCreatureStats(player).mBrutalClinchOpponent = opponent;
        opponent.getClass().getCreatureStats(opponent).mBrutalClinchOpponent = player;
        // Deliberately defer animation changes until the next actor update. This
        // request is called inside the opponent's running melee text-key callback.
        return true;
    }

    bool Actors::validSwordClinch(bool geometry, std::string* reason) const
    {
        const auto invalid = [&](std::string_view value) {
            if (reason)
                *reason = value;
            return false;
        };
        if (!mSwordClinch)
            return invalid("missing_session");
        const auto& session = *mSwordClinch;
        auto world = MWBase::Environment::get().getWorld();
        for (const MWWorld::Ptr& actor : { session.player, session.opponent })
        {
            const auto it = mIndex.find(actor.mRef);
            if (it == mIndex.end() || it->second->isInvalid() || !actor.isInCell() || !actor.getRefData().isEnabled()
                || !actor.getRefData().getBaseNode())
                return invalid("actor_unavailable");
            const auto& stats = actor.getClass().getCreatureStats(actor);
            const auto* animation = world->getAnimation(actor);
            if (session.phase == 0 && !it->second->getCharacterController().canSwordClinch())
                return invalid("entry_interrupted");
            if (stats.mBrutalClinchInterrupted)
                return invalid(stats.mBrutalClinchInterruptReason.empty() ? "interrupted" : stats.mBrutalClinchInterruptReason);
            if (stats.isDead() || stats.isParalyzed() || stats.getKnockedDown() || stats.getFatigue().getCurrent() < 0.f
                || !animation || animation->isRagdollPose() || !world->isActorCollisionEnabled(actor))
                return invalid("actor_incapacitated");
            if (stats.wasTeleported())
                return invalid("teleported");
            if (stats.getDrawState() != DrawState::Weapon)
                return invalid("weapon_sheathed");
            if (clinchSword(actor, actor == session.player)
                != (actor == session.player ? session.playerWeapon : session.opponentWeapon))
                return invalid("equipment_changed");
            if (geometry
                && (!world->isOnGround(actor) || world->isSwimming(actor) || world->isFlying(actor)
                    || !clinchGround(actor.getRefData().getPosition().asVec3())))
                return invalid("ground_changed");
        }
        if (!session.player.getClass().getCreatureStats(session.player).mBrutalClinchEnabled)
            return invalid("disabled");
        if (!geometry)
            return true;
        const osg::Vec3f playerPos = session.player.getRefData().getPosition().asVec3();
        const osg::Vec3f opponentPos = session.opponent.getRefData().getPosition().asVec3();
        if (std::abs(playerPos.z() - opponentPos.z()) > 26.f
            || (playerPos - session.playerOrigin).length2() > 28.f * 28.f
            || (opponentPos - playerPos).length2() > 195.f * 195.f)
            return invalid("displaced");
        if (!world->getLOS(session.player, session.opponent))
            return invalid("line_of_sight_blocked");
        const auto obstruction = world->getRayCasting()->castSphere(
            playerPos + osg::Vec3f(0, 0, 55), opponentPos + osg::Vec3f(0, 0, 55), 12.f, terrainMask);
        return !obstruction.mHit || invalid("pair_obstructed");
    }

    void Actors::swordClinchInput(bool pressed)
    {
        const bool rising = pressed && !mSwordClinchUseHeld;
        mSwordClinchUseHeld = pressed;
        // Binding callbacks also occur in menus before a player is registered.
        if (!pressed)
            for (Actor& actor : mActors)
                if (!actor.isInvalid())
                    actor.getPtr().getClass().getCreatureStats(actor.getPtr()).mBrutalClinchReleaseUse = false;
        if (!mSwordClinch)
            return;
        if (!pressed)
            mSwordClinch->waitingRelease = false;
        if (rising && mSwordClinch->phase <= 2 && !mSwordClinch->waitingRelease
            && !MWBase::Environment::get().getWindowManager()->isGuiMode()
            && MWBase::Environment::get().getInputManager()->getControlSwitch("playercontrols"))
        {
            // Progress starts with the parry, including pending/entry frames.
            // Only a new press counts; held input never repeats. Resolve from
            // the next mechanics update, outside the input callback.
            mSwordClinch->clicks = std::min(mSwordClinch->clicks + 1, mSwordClinch->requiredClicks);
            publishSwordClinch();
        }
    }

    void Actors::publishSwordClinch()
    {
        if (!mSwordClinch || mSwordClinch->phase == 0)
            return;
        const auto& session = *mSwordClinch;
        float length = session.duration;
        if (session.phase >= 3)
            length = clipLength(session.player, session.phase == 3 ? "brutal_clinch_win" : "brutal_clinch_lose");
        for (const MWWorld::Ptr& actor : { session.player, session.opponent })
        {
            auto& stats = actor.getClass().getCreatureStats(actor);
            stats.mBrutalClinchActive = true;
            stats.mBrutalClinchId = session.id;
            stats.mBrutalClinchOpponent = actor == session.player ? session.opponent : session.player;
            stats.mBrutalClinchPhase = session.phase;
            stats.mBrutalClinchClicks = session.clicks;
            stats.mBrutalClinchTarget = session.requiredClicks;
            stats.mBrutalClinchRemaining = std::max(0.f, length - session.age);
            stats.mBrutalClinchElapsed = session.age;
        }
    }

    void Actors::resolveSwordClinch(std::string_view result, std::string_view reason)
    {
        if (!mSwordClinch || mSwordClinch->resolved || mSwordClinch->phase == 0)
            return;
        auto& session = *mSwordClinch;
        session.resolved = true;
        auto& stats = session.player.getClass().getCreatureStats(session.player);
        stats.mBrutalClinchLastResultId = session.id;
        stats.mBrutalClinchLastResult = result;
        stats.mBrutalClinchLastReason = reason;
        MWBase::Environment::get().getLuaManager()->swordClinchResolved(
            session.player, session.opponent, session.id, result, reason, session.clicks, session.requiredClicks);
    }

    void Actors::finishSwordClinch(std::string_view reason, bool preserveRecovery)
    {
        if (!mSwordClinch)
            return;
        resolveSwordClinch("cancel", reason);
        const auto session = *mSwordClinch;
        mSwordClinch.reset();
        for (const MWWorld::Ptr& actor : { session.player, session.opponent })
        {
            if (actor.isEmpty())
                continue;
            auto& stats = actor.getClass().getCreatureStats(actor);
            stats.mBrutalClinchActive = false;
            stats.mBrutalClinchInterrupted = false;
            stats.mBrutalClinchInterruptReason.clear();
            stats.mBrutalClinchOpponent = {};
            stats.mBrutalClinchPhase = 0;
            stats.mBrutalClinchRemaining = stats.mBrutalClinchElapsed = 0.f;
            stats.mBrutalClinchVelocity = {};
            if (actor == session.player)
                stats.mBrutalClinchReleaseUse = mSwordClinchUseHeld;
            const auto it = mIndex.find(actor.mRef);
            if (it != mIndex.end() && !it->second->isInvalid())
                it->second->getCharacterController().finishSwordClinch(
                    preserveRecovery && actor == session.opponent && session.phase == 3);
            auto& movement = actor.getClass().getMovementSettings(actor);
            std::fill(std::begin(movement.mPosition), std::end(movement.mPosition), 0.f);
            std::fill(std::begin(movement.mRotation), std::end(movement.mRotation), 0.f);
            MWBase::Environment::get().getWorld()->queueMovement(actor, {});
        }
        if (session.phase == 0 && !session.opponent.getClass().getCreatureStats(session.opponent).isDead())
            session.opponent.getClass().getCreatureStats(session.opponent).mBrutalParried = true;
    }

    void Actors::cancelSwordClinch(const MWWorld::Ptr& participant, std::string_view reason)
    {
        // Gameplay events never cancel an accepted contest. Scene teardown
        // still releases references when an actor is actually removed.
        if (reason == "removed" && mSwordClinch
            && (participant == mSwordClinch->player || participant == mSwordClinch->opponent))
            finishSwordClinch(reason);
    }

    void Actors::updateSwordClinch(float duration, bool paused)
    {
        if (!mSwordClinch)
            return;
        if (paused || MWBase::Environment::get().getWindowManager()->isGuiMode())
            return;
        // Eligibility was decided by the parry. Once accepted, only Attack
        // presses and the original animation deadline decide its outcome.
        for (const auto& actor : { mSwordClinch->player, mSwordClinch->opponent })
        {
            const auto it = mIndex.find(actor.mRef);
            if (it == mIndex.end() || it->second->isInvalid() || !actor.isInCell()
                || !actor.getRefData().isEnabled() || !actor.getRefData().getBaseNode())
            {
                finishSwordClinch("actor_unavailable");
                return;
            }
        }
        auto& session = *mSwordClinch;
        auto world = MWBase::Environment::get().getWorld();
        auto& playerController = mIndex.at(session.player.mRef)->getCharacterController();
        auto& opponentController = mIndex.at(session.opponent.mRef)->getCharacterController();
        const float dt = std::max(0.f, duration);
        if (session.phase <= 3)
        {
            world->clearActorExternalVelocity(session.player);
            world->clearActorExternalVelocity(session.opponent);
        }
        if (session.phase == 0)
        {
            session.id = ++mNextSwordClinchId;
            session.phase = 1;
            session.age = 0.f;
            session.lastPlayerPosition = session.player.getRefData().getPosition().asVec3();
            session.lastOpponentPosition = session.opponent.getRefData().getPosition().asVec3();
            publishSwordClinch();
            playerController.playSwordClinch("brutal_clinch", 0.f);
            opponentController.playSwordClinch("brutal_clinch", 0.f);
            world->clearActorExternalVelocity(session.player);
            world->clearActorExternalVelocity(session.opponent);
            // As the original: at once, facing the player, on the floor below the spot, and
            // stopped short by a wall. No floor there keeps the old approach, which cannot
            // cancel the accepted pair.
            const auto* rays = world->getRayCasting();
            const osg::Vec3f from = session.lastPlayerPosition + osg::Vec3f(0, 0, 60);
            osg::Vec3f goal = session.lastPlayerPosition + session.forward * session.distance;
            const auto wall = rays->castRay(from, goal + osg::Vec3f(0, 0, 60), terrainMask);
            if (wall.mHit)
                goal = wall.mHitPos - osg::Vec3f(0, 0, 60) - session.forward * 30.f;
            const auto floor = rays->castRay(goal + osg::Vec3f(0, 0, 60), goal - osg::Vec3f(0, 0, 100), terrainMask);
            if (floor.mHit && floor.mHitNormal.z() >= .7f && clinchGround(floor.mHitPos))
            {
                session.direction = session.forward;
                session.distance = (floor.mHitPos - session.lastPlayerPosition)
                    * session.forward;
                // The original teleports. Here the same placement is a short glide, with the
                // guard still held, so the opponent never visibly jumps.
                session.placeFrom = session.lastOpponentPosition;
                session.placeTo = floor.mHitPos;
                session.placeYawFrom = session.opponent.getRefData().getPosition().rot[2];
                session.placeTime = std::clamp((session.placeTo - session.placeFrom).length() / 450.f, .12f, .25f);
                session.placeAge = 0.f;
                session.placing = true;
            }
        }
        else
            session.age += dt;

        const auto endContest = [&](bool won) {
            session.phase = won ? 3 : 4;
            session.age = 0.f;
            for (const MWWorld::Ptr& actor : { session.player, session.opponent })
            {
                actor.getClass().getCreatureStats(actor).mBrutalClinchVelocity = {};
                world->queueMovement(actor, {});
            }
            playerController.playSwordClinch(won ? "brutal_clinch_win" : "brutal_clinch_lose");
            opponentController.playSwordClinch(won ? "brutal_clinch_lose" : "brutal_clinch_win");
            resolveSwordClinch(won ? "win" : "lose", won ? "contest_complete" : "timeout");
            publishSwordClinch();
            if (!won)
            {
                // Use the normal attributed health-hit sink once. The scoped
                // native flag distinguishes this authored loss from an incoming
                // third-party hit, so it cannot cancel its own loss animation.
                const auto player = session.player;
                const auto attacker = session.opponent;
                const auto weapon = session.opponentWeapon.getCellRef().getRefId();
                struct LossDamageScope
                {
                    bool& flag;
                    explicit LossDamageScope(bool& value) : flag(value) { flag = true; }
                    ~LossDamageScope() { flag = false; }
                } lossDamage(player.getClass().getCreatureStats(player).mBrutalClinchApplyingLossDamage);
                world->addActorExternalVelocity(player, -session.direction * (400.f * 1.842655477f));
                player.getClass().onHit(player, { { "health", 10.f } }, weapon, attacker, true, DamageSourceType::Melee);
            }
        };

        if (session.phase == 1)
        {
            const float yaw = std::atan2(session.direction.x(), session.direction.y());
            bool facing = true;
            if (session.placing)
            {
                session.placeAge = std::min(session.placeAge + dt, session.placeTime);
                const float x = session.placeAge / session.placeTime;
                const float eased = x * x * (3.f - 2.f * x);
                session.opponent = world->moveObject(
                    session.opponent, session.placeFrom + (session.placeTo - session.placeFrom) * eased, true);
                const float target = session.placeYawFrom
                    + std::remainder(yaw + osg::PIf - session.placeYawFrom, 2.f * osg::PIf) * eased;
                world->rotateObject(session.opponent,
                    osg::Vec3f(0, 0, std::remainder(target - session.opponent.getRefData().getPosition().rot[2], 2.f * osg::PIf)),
                    true);
                session.opponent.getClass().getCreatureStats(session.opponent).mBrutalClinchVelocity = {};
                session.lastOpponentPosition = session.opponent.getRefData().getPosition().asVec3();
                session.placing = session.placeAge < session.placeTime;
                session.settledFrames = 0;
                session.settledTime = 0.f;
            }
            for (const MWWorld::Ptr& actor : { session.player, session.opponent })
            {
                if (session.placing && actor == session.opponent)
                    continue;
                const float targetYaw = actor == session.player ? yaw : yaw + osg::PIf;
                const auto& position = actor.getRefData().getPosition();
                const float difference = std::remainder(targetYaw - position.rot[2], 2.f * osg::PIf);
                const float pitch = actor == session.player ? -position.rot[0] : 0.f;
                facing = facing && std::abs(difference) < .035f && std::abs(pitch) < .035f;
                const float maxTurn = 7.f * dt;
                world->rotateObject(actor,
                    osg::Vec3f(std::clamp(pitch, -maxTurn, maxTurn), 0, std::clamp(difference, -maxTurn, maxTurn)),
                    true);
            }
            const osg::Vec3f playerPosition = session.player.getRefData().getPosition().asVec3();
            const osg::Vec3f opponentPosition = session.opponent.getRefData().getPosition().asVec3();
            // The previous combat movement can still be in an asynchronous
            // physics job when the parry claims the pair. Follow the player's
            // accepted position while it drains; never reposition either actor.
            session.opponentGoal = playerPosition + session.direction * session.distance;
            // Approach the reference spacing when the floor permits it. A
            // crowded street, slope or blocked step cannot invalidate a parry.
            const bool approaching = !session.placing && session.age < .65f && clinchGround(session.opponentGoal);
            osg::Vec3f correction = session.opponentGoal - opponentPosition;
            correction.z() = 0.f;
            const float distance = correction.normalize();
            auto& velocity = session.opponent.getClass().getCreatureStats(session.opponent).mBrutalClinchVelocity;
            // RefData contains accepted physics positions, not the worker's
            // pending step. A four-frame approach horizon damps that latency;
            // distance/dt overshot by a full 11 MW step at 20 FPS.
            velocity = approaching && distance > 2.f
                ? correction * std::min(220.f, distance / std::max(.15f, 4.f * dt)) : osg::Vec3f();
            const bool stationary = (playerPosition - session.lastPlayerPosition).length2() <= .25f * .25f
                && (opponentPosition - session.lastOpponentPosition).length2() <= .25f * .25f;
            if (!session.placing && (!approaching || distance <= 2.f) && facing && stationary)
            {
                ++session.settledFrames;
                session.settledTime += dt;
            }
            else
            {
                session.settledFrames = 0;
                session.settledTime = 0.f;
            }
            session.lastPlayerPosition = playerPosition;
            session.lastOpponentPosition = opponentPosition;
            // Require several accepted stationary samples after commanding zero.
            // This flushes pending locomotion before the remaining click grace.
            if (!session.placing && ((session.settledFrames >= 3 && session.settledTime >= std::max(.1f, 2.f * dt))
                || session.age >= .95f))
            {
                session.playerOrigin = playerPosition;
                session.opponentGoal = opponentPosition;
                session.phase = 2;
                session.age = 0.f;
                velocity = {};
                // Preserve the original complete hold and tempo for both actors.
                // The three-press progress is compared only at its deadline.
                playerController.playSwordClinch("brutal_clinch", 1.f);
                opponentController.playSwordClinch("brutal_clinch", 1.f);
            }
        }
        else if (session.phase == 2)
        {
            if (session.age >= session.duration)
                endContest(session.clicks >= session.requiredClicks);
        }
        else
        {
            const std::string_view group = session.phase == 3 ? "brutal_clinch_win" : "brutal_clinch_lose";
            if (session.age >= std::max(.1f, clipLength(session.player, group)))
            {
                finishSwordClinch("complete", true);
                return;
            }
        }
        publishSwordClinch();
    }
}
