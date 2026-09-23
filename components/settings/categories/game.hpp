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
        // Use optional player-only Curvy Body equipment meshes when present in the VFS.
        SettingValue<bool> mCurvyBodyMeshes{ mIndex, "Game", "curvy body meshes" };
        // Use optional player-only naked female BBR body mesh when fully unequipped.
        SettingValue<bool> mCurvyNakedBody{ mIndex, "Game", "curvy naked body" };
        SettingValue<bool> mJiggleBoneDebug{ mIndex, "Game", "jiggle bone debug" };
        // Restrict all jiggle (auto-rig, controllers, thigh) to the player character only; NPCs
        // get no jiggle. Applies when a body is next loaded (reload a save or re-equip).
        SettingValue<bool> mJiggleBonePlayerOnly{ mIndex, "Game", "jiggle player only" };
        // Keep the simulation running (physics, jiggle, NPCs, time) while the Options/Settings
        // window is open, instead of pausing - so movement and jiggle can be previewed live while
        // adjusting sliders. The console, interactive message boxes, and having no active game
        // still pause as normal.
        SettingValue<bool> mOptionsMenuRealTime{ mIndex, "Game", "options menu real time" };
        // Damped-spring tuning for breast/butt jiggle-bone secondary motion.
        SettingValue<float> mJiggleBoneStiffness{ mIndex, "Game", "jiggle bone stiffness",
            makeClampSanitizerFloat(1.f, 1000.f) };
        SettingValue<float> mJiggleBoneDamping{ mIndex, "Game", "jiggle bone damping",
            makeClampSanitizerFloat(0.f, 100.f) };
        SettingValue<float> mJiggleBoneMaxDisplacement{ mIndex, "Game", "jiggle bone max displacement",
            makeClampSanitizerFloat(0.f, 50.f) };
        SettingValue<float> mJiggleBoneIntensity{ mIndex, "Game", "jiggle bone intensity",
            makeClampSanitizerFloat(0.f, 20.f) };
        // --- TittyMagic-style physics feel (ported from everlaster's VaM plugin) ---
        // World-space gravity pull on the jiggle spring, in units/s^2. Makes the bone sag at
        // rest and swing like a pendulum, so it responds to body orientation and motion the
        // way TittyMagic's "gravity physics" does (sag when upright, swing forward/back/sideways
        // when leaning) - here it emerges from a single real gravity term rather than authored
        // pitch/roll curves. 0 = the old behaviour (no sag, motion-lag only).
        SettingValue<float> mJiggleBoneGravity{ mIndex, "Game", "jiggle bone gravity",
            makeClampSanitizerFloat(0.f, 2000.f) };
        // Softness master (0-100), analogous to TittyMagic's breastSoftness. Higher = looser and
        // bouncier: scales the effective spring and damping down together, so the bone jiggles
        // more freely and settles slower.
        SettingValue<float> mJiggleBoneSoftness{ mIndex, "Game", "jiggle bone softness",
            makeClampSanitizerFloat(0.f, 100.f) };
        // Quickness master (0-100), analogous to TittyMagic's breastQuickness. Higher = snappier:
        // raises the effective spring so the bone reacts and returns faster.
        SettingValue<float> mJiggleBoneQuickness{ mIndex, "Game", "jiggle bone quickness",
            makeClampSanitizerFloat(0.f, 100.f) };
        // How strongly a bone's own size (its rest protrusion from the parent joint) affects the
        // feel, analogous to TittyMagic deriving physics from breast mass/volume (InvertMass).
        // Higher = larger/heavier bulges get a softer spring, more damping and thus more sag/swing.
        // 0 = size-independent.
        SettingValue<float> mJiggleBoneMassResponse{ mIndex, "Game", "jiggle bone mass response",
            makeClampSanitizerFloat(0.f, 2.f) };
        // Side-sway: scales the horizontal (side-to-side / forward-back) part of the jiggle
        // relative to the up-down bounce. 1.0 = uniform; 0 = vertical only; >1 = looser sway.
        SettingValue<float> mJiggleBoneSide{ mIndex, "Game", "jiggle bone side",
            makeClampSanitizerFloat(0.f, 3.f) };
        // Self/body-collision containment: when on, the simulated bone cannot sink toward the body
        // (its parent joint) past "self collision limit" units, and its inward velocity is killed at
        // that wall - the bone-space analog of TittyMagic's soft self-collision + distance limit, so
        // jiggle/sag can't clip the bulge back through the ribcage.
        SettingValue<bool> mJiggleBoneSelfCollision{ mIndex, "Game", "jiggle bone self collision" };
        SettingValue<float> mJiggleBoneSelfCollisionLimit{ mIndex, "Game", "jiggle bone self collision limit",
            makeClampSanitizerFloat(0.f, 50.f) };
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
        // Also auto-rig L/R thigh jiggle bones (identity children of the real Bip01 L/R Thigh bones),
        // cone-painted onto the upper-thigh flesh, so the thighs jiggle with the same spring/gravity/
        // softness settings as the breasts. Only applies while "jiggle auto rig" is on.
        SettingValue<bool> mJiggleThigh{ mIndex, "Game", "jiggle thigh" };
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
        // Runtime "tes3cmd clean" for plugins (applied in-memory at load; restart to change).
        // When on, an "Evil GMST" (one of the 72 Tribunal/Bloodmoon GMSTs the Construction Set
        // injects into plugins with its own default value) is dropped when a plugin re-adds it
        // with that exact evil value, so the base game/expansion value is preserved. GMSTs a mod
        // intentionally changed to a different value are kept.
        SettingValue<bool> mCleanPlugins{ mIndex, "Game", "clean plugins" };
    };
}

#endif
