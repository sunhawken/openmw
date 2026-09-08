#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_GAME_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_GAME_H

#include <components/detournavigator/collisionshapetype.hpp>
#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

#include <osg/Math>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Settings
{
    struct GameCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<int> mShowOwned{ mIndex, "Game", "show owned", makeEnumSanitizerInt({ 0, 1, 2, 3 }) };
        SettingValue<bool> mShowProjectileDamage{ mIndex, "Game", "show projectile damage" };
        SettingValue<bool> mShowMeleeInfo{ mIndex, "Game", "show melee info" };
        SettingValue<bool> mShowEnchantChance{ mIndex, "Game", "show enchant chance" };
        SettingValue<bool> mBestAttack{ mIndex, "Game", "best attack" };
        SettingValue<int> mDifficulty{ mIndex, "Game", "difficulty", makeClampSanitizerInt(-500, 500) };
        // We have to cap it since using high values (larger than 7168) will make some quests harder or impossible to
        // complete (bug #1876)
        SettingValue<int> mActorsProcessingRange{ mIndex, "Game", "actors processing range",
            makeClampSanitizerInt(3584, 7168) };
        SettingValue<bool> mClassicReflectedAbsorbSpellsBehavior{ mIndex, "Game",
            "classic reflected absorb spells behavior" };
        SettingValue<bool> mClassicCalmSpellsBehavior{ mIndex, "Game", "classic calm spells behavior" };
        SettingValue<bool> mShowEffectDuration{ mIndex, "Game", "show effect duration" };
        SettingValue<bool> mPreventMerchantEquipping{ mIndex, "Game", "prevent merchant equipping" };
        SettingValue<bool> mEnchantedWeaponsAreMagical{ mIndex, "Game", "enchanted weapons are magical" };
        SettingValue<bool> mFollowersAttackOnSight{ mIndex, "Game", "followers attack on sight" };
        SettingValue<bool> mCanLootDuringDeathAnimation{ mIndex, "Game", "can loot during death animation" };
        SettingValue<bool> mRebalanceSoulGemValues{ mIndex, "Game", "rebalance soul gem values" };
        SettingValue<bool> mUseAdditionalAnimSources{ mIndex, "Game", "use additional anim sources" };
        SettingValue<bool> mSmoothAnimTransitions{ mIndex, "Game", "smooth animation transitions" };
        SettingValue<bool> mBarterDispositionChangeIsPermanent{ mIndex, "Game",
            "barter disposition change is permanent" };
        SettingValue<int> mStrengthInfluencesHandToHand{ mIndex, "Game", "strength influences hand to hand",
            makeEnumSanitizerInt({ 0, 1, 2 }) };
        SettingValue<bool> mWeaponSheathing{ mIndex, "Game", "weapon sheathing" };
        SettingValue<bool> mShieldSheathing{ mIndex, "Game", "shield sheathing" };
        SettingValue<bool> mOnlyAppropriateAmmunitionBypassesResistance{ mIndex, "Game",
            "only appropriate ammunition bypasses resistance" };
        SettingValue<bool> mUseMagicItemAnimations{ mIndex, "Game", "use magic item animations" };
        SettingValue<bool> mNormaliseRaceSpeed{ mIndex, "Game", "normalise race speed" };
        SettingValue<float> mProjectilesEnchantMultiplier{ mIndex, "Game", "projectiles enchant multiplier",
            makeClampSanitizerFloat(0, 1) };
        SettingValue<bool> mUncappedDamageFatigue{ mIndex, "Game", "uncapped damage fatigue" };
        SettingValue<bool> mTurnToMovementDirection{ mIndex, "Game", "turn to movement direction" };
        SettingValue<bool> mSmoothMovement{ mIndex, "Game", "smooth movement" };
        SettingValue<float> mSmoothMovementPlayerTurningDelay{ mIndex, "Game", "smooth movement player turning delay",
            makeMaxSanitizerFloat(0.01f) };
        SettingValue<bool> mNPCsAvoidCollisions{ mIndex, "Game", "NPCs avoid collisions" };
        SettingValue<bool> mNPCsGiveWay{ mIndex, "Game", "NPCs give way" };
        SettingValue<bool> mSwimUpwardCorrection{ mIndex, "Game", "swim upward correction" };
        SettingValue<float> mSwimUpwardCoef{ mIndex, "Game", "swim upward coef", makeClampSanitizerFloat(-1, 1) };
        SettingValue<bool> mTrainersTrainingSkillsBasedOnBaseSkill{ mIndex, "Game",
            "trainers training skills based on base skill" };
        SettingValue<bool> mAlwaysAllowStealingFromKnockedOutActors{ mIndex, "Game",
            "always allow stealing from knocked out actors" };
        SettingValue<bool> mGraphicHerbalism{ mIndex, "Game", "graphic herbalism" };
        SettingValue<bool> mAllowActorsToFollowOverWaterSurface{ mIndex, "Game",
            "allow actors to follow over water surface" };
        SettingValue<osg::Vec3f> mDefaultActorPathfindHalfExtents{ mIndex, "Game",
            "default actor pathfind half extents", makeMaxStrictSanitizerVec3f(osg::Vec3f(0, 0, 0)) };
        SettingValue<bool> mDayNightSwitches{ mIndex, "Game", "day night switches" };
        SettingValue<DetourNavigator::CollisionShapeType> mActorCollisionShapeType{ mIndex, "Game",
            "actor collision shape type" };
        SettingValue<bool> mPlayerMovementIgnoresAnimation{ mIndex, "Game", "player movement ignores animation" };
        SettingValue<bool> mJiggleBoneDebug{ mIndex, "Game", "jiggle bone debug" };
        // Damped-spring tuning for breast/butt jiggle-bone secondary motion.
        SettingValue<float> mJiggleBoneStiffness{ mIndex, "Game", "jiggle bone stiffness",
            makeClampSanitizerFloat(1.f, 1000.f) };
        SettingValue<float> mJiggleBoneDamping{ mIndex, "Game", "jiggle bone damping",
            makeClampSanitizerFloat(0.f, 100.f) };
        SettingValue<float> mJiggleBoneMaxDisplacement{ mIndex, "Game", "jiggle bone max displacement",
            makeClampSanitizerFloat(0.f, 50.f) };
        SettingValue<float> mJiggleBoneIntensity{ mIndex, "Game", "jiggle bone intensity",
            makeClampSanitizerFloat(0.f, 20.f) };
        // Extra static Z (up/down) offset applied to the breast/butt jiggle bones' rest
        // position on top of whatever bind pose their NIF already has - lets a rigged
        // mesh's bulge placement be nudged live without re-rigging/re-exporting the NIF.
        SettingValue<float> mJiggleBoneBreastZOffset{ mIndex, "Game", "jiggle bone breast z offset",
            makeClampSanitizerFloat(-50.f, 50.f) };
        SettingValue<float> mJiggleBoneButtZOffset{ mIndex, "Game", "jiggle bone butt z offset",
            makeClampSanitizerFloat(-50.f, 50.f) };
        // Per-body-mesh breast/butt Z offsets, saved from the in-game sliders and keyed by the
        // player's body mesh so each mesh remembers its own tuning. Entries are "meshpath=breast,butt".
        SettingValue<std::vector<std::string>> mJiggleMeshZOffsets{ mIndex, "Game", "jiggle mesh z offsets" };
        // In-engine auto jiggle rigger: procedurally add breast/butt jiggle bones + weights to
        // female body meshes at load that don't already have them (so the .bat pre-rig is optional).
        SettingValue<bool> mJiggleAutoRig{ mIndex, "Game", "jiggle auto rig" };
        // NIF-filename substrings (lowercase, comma-separated) the auto-rigger must skip entirely:
        // such meshes are excluded from both anchor detection and weight painting, so an odd armor
        // can neither get bad jiggle nor pollute the shared body anchor.
        SettingValue<std::vector<std::string>> mJiggleAutoRigBlacklist{ mIndex, "Game", "jiggle auto rig blacklist" };
        // Log auto-rigger anchor/weight detection (throttled).
        SettingValue<bool> mJiggleAutoRigDebug{ mIndex, "Game", "jiggle auto rig debug" };
        // Auto seam welder: runtime-welds boundary vertices of rigid meshes (armor/clothing
        // that lack jiggle bones) onto the moving jiggle-mesh vertices they were coincident
        // with at rest pose, closing the visual cracks jiggle otherwise opens at those seams.
        SettingValue<bool> mJiggleSeamWelding{ mIndex, "Game", "jiggle seam welding" };
        // Log detection/correspondence details for the seam welder (bone classification and
        // welded-vertex counts). Throttled.
        SettingValue<bool> mJiggleSeamWeldDebug{ mIndex, "Game", "jiggle seam weld debug" };
        // Two vertices from different meshes are treated as the same seam point when their
        // rest-pose (bind) positions are within this distance, in world units.
        SettingValue<float> mJiggleSeamWeldThreshold{ mIndex, "Game", "jiggle seam weld threshold",
            makeClampSanitizerFloat(0.01f, 10.f) };
        // Caps how many enemies can newly enter combat against the player at once (0 = no
        // cap). Only throttles new enemies engaging the player - actors already fighting
        // each other are unaffected, and this does not apply to allies (see below).
        SettingValue<int> mMaxActorsInCombatWithPlayer{ mIndex, "Game", "max actors in combat with player",
            makeClampSanitizerInt(0, 50) };
        // Caps how many actors can newly enter combat against a target that ISN'T the player
        // (0 = no cap) - i.e. allies/guards/companions piling onto something to help the
        // player, as opposed to enemies piling onto the player. Independent of the setting
        // above.
        SettingValue<int> mMaxAlliesInCombat{ mIndex, "Game", "max allies in combat",
            makeClampSanitizerInt(0, 50) };
    };
}

#endif
