#ifndef GAME_MWMECHANICS_CREATURESTATS_H
#define GAME_MWMECHANICS_CREATURESTATS_H

#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

#include <osg/Vec3f>

#include "activespells.hpp"
#include "aisequence.hpp"
#include "aisetting.hpp"
#include "drawstate.hpp"
#include "magiceffects.hpp"
#include "spells.hpp"
#include "stat.hpp"
#include "../mwworld/ptr.hpp"

#include <components/esm/attr.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/magiceffects.hpp>

namespace ESM
{
    struct CreatureStats;
}

namespace MWMechanics
{
    struct CorprusStats
    {
        static constexpr int sWorseningPeriod = 24;

        int mWorsenings[ESM::Attribute::Length];
        MWWorld::TimeStamp mNextWorsening;
    };

    /// \brief Common creature stats
    ///
    ///
    class CreatureStats
    {
        std::map<ESM::RefId, AttributeValue> mAttributes;
        DynamicStat<float> mDynamic[3]; // health, magicka, fatigue
        DrawState mDrawState = DrawState::Nothing;
        Spells mSpells;
        ActiveSpells mActiveSpells;
        MagicEffects mMagicEffects;
        Stat<int> mAiSettings[4];
        AiSequence mAiSequence;
        bool mDead = false;
        bool mDeathAnimationFinished = false;
        bool mDied = false; // flag for OnDeath script function
        bool mMurdered = false;
        int mFriendlyHits = 0;
        bool mTalkedTo = false;
        bool mAlarmed = false;
        bool mAttacked = false;
        bool mKnockdown = false;
        bool mKnockdownOneFrame = false;
        bool mKnockdownOverOneFrame = false;
        bool mHitRecovery = false;
        bool mBlock = false;
        unsigned int mMovementFlags = 0;

        float mFallHeight = 0.f;

        ESM::RefId mLastHitObject; // The last object to hit this actor
        ESM::RefId mLastHitAttemptObject; // The last object to attempt to hit this actor

        // For merchants: the last time items were restocked and gold pool refilled.
        MWWorld::TimeStamp mLastRestock;

        // The pool of merchant gold (not in inventory)
        int mGoldPool = 0;

        // Stores an actor that attacked this actor. Only one is stored at a time, and it is not changed if a different
        // actor attacks. It is cleared when combat ends.
        ESM::RefNum mHitAttemptActor;

        // The difference between view direction and lower body direction.
        float mSideMovementAngle = 0;

        MWWorld::TimeStamp mTimeOfDeath;

    private:
        std::multimap<ESM::RefId, ESM::RefNum> mSummonedCreatures; // <Effect, Actor>

        float mAwarenessTimer = 0.f;
        int mAwarenessRoll = -1;

    protected:
        std::string mAttackType;
        int mLevel = 0;
        bool mAttackingOrSpell = false;

    private:
        // The index of the death animation that was played, or -1 if none played
        signed char mDeathAnimation = -1;

        bool mTeleported = false;

    public:
        CreatureStats();

        struct PowerStrike
        {
            std::string token;
            MWWorld::Ptr weapon;
            float multiplier = 1.f;
            float minimumStrength = 0.8f;
        };
        PowerStrike mPowerStrike;
        std::string mLastConsumedPowerStrikeToken;
        // Transient Lua presentation state: defer automatic arrest while adrenaline is visible.
        bool mBrutalAdrenalineEffectActive = false;
        bool mBrutalBlockRequested = false; // Player is holding the block control (Lua).
        bool mBrutalBlockRaised = false; // Controller-owned: the shield is up this frame.
        bool mBrutalBlockImpact = false; // A blow landed on the raised shield; consumed by the controller.
        bool mBrutalParry = false; // Controller-owned: the raised guard is a weapon parry (no shield).
        double mBrutalBlockRaisedAt = -1.0; // Simulation time the current guard started rising.
        double mBrutalBlockLoweredAt = -1e9; // Simulation time the previous guard ended.
        int mBrutalParryResult = 0; // BrutalParry; set by blockMeleeAttack, consumed by the controller.
        bool mBrutalOffhandStrike = false; // The current native melee hit is the left dagger's.
        bool mBrutalFastStrike = false; // The current native melee hit is a chain click: no finisher, less adrenaline.
        float mBrutalComboMultiplier = 1.f; // Damage ratio of the combo strike being delivered.
        bool mBrutalShieldRush = false; // Snapshot onHit before leaving the rush hit key.
        bool mBrutalSlashRight = false; // Latched source _R direction of the ordinary player slash.
        bool mBrutalSlideAttack = false; // This hand slash began during a slide; retained through its hit/end.
        bool mBrutalSlideRequested = false;
        bool mBrutalSliding = false; // IN/LOOP: physics keeps momentum without wish acceleration.
        bool mBrutalSlideLow = false; // Keep the low hull through the exit animation.
        bool mBrutalSlideBlocked = false; // Physics cannot safely restore the standing hull.
        bool mBrutalSlideDownhill = false; // Last accepted physics step: supported downhill travel.
        osg::Vec3f mBrutalSlideGroundNormal;
        osg::Vec3f mBrutalSlideGroundPoint; // Contact plane for the separate visual body; not a movement target.
        int mBrutalSlidePhase = 0; // 0 idle, 1 entering, 2 sliding, 3 rising.
        float mBrutalSlideBlend = 0.f;
        // Transient paired combat state; neither configuration nor locks enter a save.
        bool mBrutalClinchEnabled = false;
        int mBrutalClinchRequiredClicks = 3;
        float mBrutalClinchDuration = 1.4333333f;
        float mBrutalClinchDistance = 81.07684f;
        bool mBrutalClinchActive = false;
        bool mBrutalClinchInterrupted = false;
        bool mBrutalClinchApplyingLossDamage = false; // Scoped native hit; never exposed to Lua.
        std::string mBrutalClinchInterruptReason;
        bool mBrutalClinchReleaseUse = false;
        float mBrutalClinchBlendOut = 0.f;
        MWWorld::Ptr mBrutalClinchOpponent;
        uint64_t mBrutalClinchId = 0;
        int mBrutalClinchPhase = 0; // 0 idle, 1 aligning, 2 contest, 3 win, 4 lose.
        int mBrutalClinchClicks = 0;
        int mBrutalClinchTarget = 0;
        float mBrutalClinchRemaining = 0.f;
        float mBrutalClinchElapsed = 0.f;
        uint64_t mBrutalClinchLastResultId = 0;
        std::string mBrutalClinchLastResult;
        std::string mBrutalClinchLastReason;
        osg::Vec3f mBrutalClinchVelocity; // World-space collision-resolved alignment only.
        bool mBrutalParried = false; // NPC: the player timed a parry of this actor's blow; recoil next update.
        // Kinetic player gait chosen by Lua (0 = Morrowind run stance, 1 walk, 2 run, 3 sprint).
        int mBrutalGait = 0;


        DrawState getDrawState() const;
        void setDrawState(DrawState state);

        void recalculateMagicka();

        float getFallHeight() const;
        void addToFallHeight(float height);

        /// Reset the fall height
        /// @return total fall height
        float land(bool isPlayer = false);

        const AttributeValue& getAttribute(ESM::RefId id) const;

        const DynamicStat<float>& getHealth() const;

        const DynamicStat<float>& getMagicka() const;

        const DynamicStat<float>& getFatigue() const;

        const DynamicStat<float>& getDynamic(int index) const;

        const Spells& getSpells() const;

        const ActiveSpells& getActiveSpells() const;

        const MagicEffects& getMagicEffects() const;

        bool getAttackingOrSpell() const { return mAttackingOrSpell; }
        std::string_view getAttackType() const { return mAttackType; }

        int getLevel() const;

        Spells& getSpells();

        ActiveSpells& getActiveSpells();

        MagicEffects& getMagicEffects();

        void setAttribute(ESM::RefId id, const AttributeValue& value);
        // Shortcut to set only the base
        void setAttribute(ESM::RefId id, float base);

        void setHealth(const DynamicStat<float>& value);

        void setMagicka(const DynamicStat<float>& value);

        void setFatigue(const DynamicStat<float>& value);

        void setDynamic(int index, const DynamicStat<float>& value);

        void setAttackingOrSpell(bool attackingOrSpell) { mAttackingOrSpell = attackingOrSpell; }

        void setAttackType(std::string_view attackType) { mAttackType = attackType; }

        void setLevel(int level);

        void setAiSetting(AiSetting index, Stat<int> value);
        void setAiSetting(AiSetting index, int base);
        Stat<int> getAiSetting(AiSetting index) const;

        const AiSequence& getAiSequence() const;

        AiSequence& getAiSequence();

        float getFatigueTerm() const;
        ///< Return effective fatigue

        bool isParalyzed() const;

        bool isDead() const;

        bool isDeathAnimationFinished() const;
        void setDeathAnimationFinished(bool finished);

        void notifyDied();

        bool hasDied() const;

        void clearHasDied();

        bool hasBeenMurdered() const;

        void clearHasBeenMurdered();

        void notifyMurder();

        void resurrect();

        bool hasCommonDisease() const;

        bool hasBlightDisease() const;

        int getFriendlyHits() const;
        ///< Number of friendly hits received.

        void friendlyHit();
        ///< Increase number of friendly hits by one.

        void resetFriendlyHits();

        bool hasTalkedToPlayer() const;
        ///< Has this creature talked with the player before?

        void talkedToPlayer();

        bool isAlarmed() const;
        void setAlarmed(bool alarmed);

        bool getAttacked() const;
        void setAttacked(bool attacked);

        float getEvasion() const;

        void setKnockedDown(bool value);
        /// Returns true for the entire duration of the actor being knocked down or knocked out,
        /// including transition animations (falling down & standing up)
        bool getKnockedDown() const;
        void setKnockedDownOneFrame(bool value);
        /// Returns true only for the first frame of the actor being knocked out; used for "onKnockedOut" command
        bool getKnockedDownOneFrame() const;
        void setKnockedDownOverOneFrame(bool value);
        /// Returns true for all but the first frame of being knocked out; used to know to not reset
        /// mKnockedDownOneFrame
        bool getKnockedDownOverOneFrame() const;
        void setHitRecovery(bool value);
        bool getHitRecovery() const;
        void setBlock(bool value);
        bool getBlock() const;

        std::multimap<ESM::RefId, ESM::RefNum>& getSummonedCreatureMap(); // <Effect, summoned creature>

        enum Flag
        {
            Flag_ForceRun = 1,
            Flag_ForceSneak = 2,
            Flag_Run = 4,
            Flag_Sneak = 8,
            Flag_ForceJump = 16,
            Flag_ForceMoveJump = 32
        };
        enum Stance
        {
            Stance_Run,
            Stance_Sneak
        };

        bool getMovementFlag(Flag flag) const;
        void setMovementFlag(Flag flag, bool state);
        /// Like getMovementFlag, but also takes into account if the flag is Forced
        bool getStance(Stance flag) const;

        void setLastHitObject(const ESM::RefId& objectid);
        void clearLastHitObject();
        const ESM::RefId& getLastHitObject() const;
        void setLastHitAttemptObject(const ESM::RefId& objectid);
        void clearLastHitAttemptObject();
        const ESM::RefId& getLastHitAttemptObject() const;
        void setHitAttemptActor(ESM::RefNum actorId);
        ESM::RefNum getHitAttemptActor() const;

        void writeState(ESM::CreatureStats& state) const;

        void readState(const ESM::CreatureStats& state);

        void setLastRestockTime(MWWorld::TimeStamp tradeTime);
        MWWorld::TimeStamp getLastRestockTime() const;

        void setGoldPool(int pool);
        int getGoldPool() const;

        signed char getDeathAnimation() const; // -1 means not decided
        void setDeathAnimation(signed char index);

        MWWorld::TimeStamp getTimeOfDeath() const;

        float getSideMovementAngle() const { return mSideMovementAngle; }
        void setSideMovementAngle(float angle) { mSideMovementAngle = angle; }

        bool wasTeleported() const { return mTeleported; }
        void setTeleported(bool v) { mTeleported = v; if (v) mPowerStrike = {}; }

        const std::map<ESM::RefId, AttributeValue>& getAttributes() const { return mAttributes; }

        void updateAwareness(float duration);
        int getAwarenessRoll();
    };
}

#endif
