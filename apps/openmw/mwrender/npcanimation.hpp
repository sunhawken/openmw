#ifndef GAME_RENDER_NPCANIMATION_H
#define GAME_RENDER_NPCANIMATION_H

#include "actoranimation.hpp"
#include "animation.hpp"
#include "weaponanimation.hpp"

#include <components/vfs/pathutil.hpp>

#include "../mwworld/inventorystore.hpp"

#include <array>
#include <vector>
#include <cstdint>
#include <osg/observer_ptr>

namespace ESM
{
    struct NPC;
    struct BodyPart;
    struct ReleasedEquipmentState;
}

namespace MWSound
{
    class Sound;
}

namespace MWRender
{

    class RotateController;
    class HeadAnimationTime;
    struct DecapitationGeometry;
    struct BisectionGeometry;
    struct LimbSeveringGeometry;
    class ClippedRenderPrewarm;

    class NpcAnimation : public ActorAnimation, public WeaponAnimation, public MWWorld::InventoryStoreListener
    {
    public:
        void equipmentChanged() override;

    public:
        typedef std::map<ESM::PartReferenceType, std::string> PartBoneMap;

        enum ViewMode
        {
            VM_Normal,
            VM_FirstPerson,
//## VR_PATCH BEGIN
            VM_HeadOnly,
            VM_VRNormal,
            VM_VRFirstPerson,
        };

        struct SwordWallContact
        {
            bool armed = false, pending = false, soundPlayed = false;
            std::uint64_t count = 0, swing = 0, sounds = 0;
            unsigned int frame = 0, samples = 0;
            float sourceTime = 0.f, sweepFraction = 0.f;
            unsigned int sweepSegments = 0, sweepCandidates = 0, sweepConfirmed = 0;
            float sweepMilliseconds = 0.f;
            std::string key, kind, mount;
            osg::Vec3f position, normal, previousBase, previousTip, bladeBase, bladeTip, sweepFrom, sweepTo;
            osg::Vec3f currentBase, currentTip, currentContactPoint;
        };

        // Read-only controller snapshot for comparing real input and rendered
        // weapon lifecycles. It never selects or advances an animation.
        struct WeaponCycleState
        {
            std::string_view phase = "none";
            int index = 0, upperBody = 0;
            bool queued = false, button = false, readyToHit = false;
            float held = 0.f;
            bool chargedHold = false, guardCharge = false;
            float useHeld = 0.f;
            std::uint64_t acceptedStrikes = 0;
        };

    private:
        static const PartBoneMap sPartList;

        // Opt-in private geometry audit. The callback samples after the whole
        // object update traversal, after the rendered ragdoll bones are assigned.
        struct GarmentAudit;
        std::unique_ptr<GarmentAudit> mGarmentAudit;
        void updateRagdollGarmentAudit(float timepassed);
        void dumpRagdollGarmentForAudit(osg::NodeVisitor& visitor);
        struct GarmentCoverage;
        struct BloodDecals;
        std::shared_ptr<BloodDecals> mBloodDecals;
        struct CutBloodFlow;
        std::shared_ptr<CutBloodFlow> mCutBloodFlow;
        void updateCutBloodFlow(float dt);
        void emitCutBloodFlow(osg::NodeVisitor& visitor);
        void clearCutBloodFlow();
        void rebuildBloodDecals();
        bool useBrutalStaffSet() const override;
        void prewarmBloodDecals();
        void transferBloodDecalsToHead(DecapitationGeometry& target, bool bindSources = false);
        void transferBloodDecalsToTorso(BisectionGeometry& target);
        void transferBloodDecalsToLimbs(LimbSeveringGeometry& target);
        std::unique_ptr<GarmentCoverage> mGarmentCoverage;
        bool mGarmentCoveragePrepared = false, mGarmentWasRagdoll = false;
        double mGarmentRagdollAge = 0.;
        void clearRagdollGarmentCoverage();
        void prepareRagdollGarmentCoverage();
        void updateRagdollGarmentCoverage(float timepassed);
        void evaluateRagdollGarmentCoverage(osg::NodeVisitor& visitor);

        std::unique_ptr<DecapitationGeometry> mDecapitation;
        std::unique_ptr<BisectionGeometry> mBisection;
        std::unique_ptr<LimbSeveringGeometry> mLimbSevering;
        std::array<bool, ESM::PRT_Count> mLimbReplacedParts{};
        int mLimbSeveringFailure = 0;
        bool rebuildSeveredLimbs(unsigned mask);
        int mBisectionFailure = 0;
        bool rebuildSeveredTorso();
        bool rebuildSeveredHead(bool prewarmOnly = false);
        int mDecapitationFailure = 0;
        osg::ref_ptr<osg::StateSet> mDecapitationCapState;
        osg::ref_ptr<osg::StateSet> mDecapitationHeadCapState;
        bool prewarmDecapitationMaterial();
        std::size_t mDecapitationPrewarmQueued = 0;
        std::unique_ptr<ClippedRenderPrewarm> mDecapitationRenderPrewarm;
        std::size_t mDecapitationWarmDrawn = 0, mDecapitationWarmTimedOut = 0, mDecapitationWarmDrawables = 0;

        // Bounded Parts
        PartHolderPtr mObjectParts[ESM::PRT_Count];
        std::array<std::string, ESM::PRT_Count> mPartModels;
        std::array<std::string, ESM::PRT_Count> mFirstPersonWristCoverage;
        std::array<MWSound::Sound*, ESM::PRT_Count> mSounds;

        const ESM::NPC* mNpc;
        VFS::Path::Normalized mHeadModel;
        VFS::Path::Normalized mHairModel;
        ViewMode mViewMode;
        bool mShowWeapons;
        bool mShowCarriedLeft;

        enum NpcType
        {
            Type_Normal,
            Type_Werewolf,
            Type_Vampire
        };
        NpcType mNpcType;

        int mPartslots[ESM::PRT_Count]; // Each part slot is taken by clothing, armor, or is empty
        int mPartPriorities[ESM::PRT_Count];

        // Source matrices are restored before every update and blend capture. The
        // fit is applied only after the whole scene has evaluated its current pose.
        std::vector<std::pair<osg::observer_ptr<osg::MatrixTransform>, osg::Matrixf>> mClinchSourcePose;
        osg::observer_ptr<osg::Node> mClinchBladeNode;
        osg::Vec3f mClinchBladeRange;
        std::uint64_t mClinchPoseId = 0;
        bool mClinchWasContest = false;
        double mClinchEntryTime = 0., mClinchExitTime = 0.;
        osg::Vec3f mClinchLastCorrection, mClinchExitCorrection;
        osg::Vec3f getClinchBladeRange();

        SwordWallContact mSwordWallContact;
        WeaponCycleState mWeaponCycleState;
        struct WallWeaponState
        {
            osg::observer_ptr<osg::Node> node;
            std::vector<osg::Vec3f> vertices;
            std::vector<std::pair<std::size_t, std::size_t>> edges;
            osg::Matrixf previousMatrix, previousCamera;
            osg::Vec3f previousRootScale{ 1.f, 1.f, 1.f }, previousRootPosition;
            osg::Quat previousRootAttitude;
            float previousFovRatio = 1.f;
            osg::Vec3f materialPoint;
            bool materialWitness = false;
            float recoilAge = -1.f;
            osg::Vec3f recoilFoldAxis;
            int recoilFoldDegrees = 0;
            bool recoilFoldValid = false;
            osg::Vec3f previousPosition;
            float base = 0.f, tip = 0.f, previousTime = -1.f;
            bool history = false;
        };
        std::array<WallWeaponState, 2> mWallWeapons;
        unsigned mWallStruckMount = 0;
        std::string mSwordWallGroup, mSwordWallKey;
        std::vector<std::pair<osg::observer_ptr<osg::MatrixTransform>, osg::Matrixf>> mSwordWallSourcePose;
        bool mSwordWallStruck = false;
        osg::Matrixf mSwordWallLastCorrection, mSwordWallAppliedTail;
        osg::Vec3f mSwordWallCorrectionOrigin;
        float mSwordWallTailAge = -1.f;
        bool mSwordWallCorrectionValid = false;
        bool applySwordWallCorrection(const osg::Matrixf& correction, const osg::FrameStamp& frameStamp);
        bool sampleWeaponWallContact(unsigned mount, osg::Node* node, float duration, const osg::FrameStamp& frameStamp);

        osg::Vec3f mFirstPersonOffset;
        // Field of view to use when rendering first person meshes
        float mFirstPersonFieldOfView;

        std::shared_ptr<HeadAnimationTime> mHeadAnimationTime;
        std::shared_ptr<WeaponAnimationTime> mWeaponAnimationTime;

        bool mSoundsDisabled;

        // A secondary normal skeleton supplies only the world-space torso and legs.
        // It has no inventory/text-key listener and cannot drive combat or actor motion.
        bool mBodyAwareness = false;
        bool mBodyAwarenessHiddenForClinch = false;
        void updateBodyAwarenessClinchVisibility();
        osg::ref_ptr<osg::MatrixTransform> mBodyAwarenessRoot;
        osg::ref_ptr<NpcAnimation> mBodyAwarenessAnimation;
        osg::Vec3f mBodyGroundNormal{ 0.f, 0.f, 1.f };
        osg::Vec3f mBodyGroundUp{ 0.f, 0.f, 1.f };
        osg::Vec3f mBodyGroundAnchor;
        float mBodyGroundOffset = 0.f;
        float mBodyGroundLift = 0.f;
        float mBodyGroundBlend = 0.f;
        void resetBodyAwareness();
        void updateBodyAwareness();
        void syncBodyAwareness(float duration);
        void alignBodyAwareness(float duration);

        bool mAccurateAiming;
        enum BrutalViewmodel
        {
            BrutalViewmodel_Default,
            BrutalViewmodel_Shield,
            BrutalViewmodel_Staff,
            BrutalViewmodel_Dagger,
        };
        static constexpr std::string_view sBrutalLeftWeaponBone = "Brutal Left Weapon";
        BrutalViewmodel mViewmodelSet = BrutalViewmodel_Default; // Clip set loaded for the first-person rig.
        BrutalViewmodel brutalViewmodelSet() const;
        bool carriesLeftWeapon() const;
        bool carriesShield() const;
        float mAimingFactor;

        // Hazaeki-style transformation support. When a Hazaeki player is turned into a "humanoid
        // form" (the mod sets the MWScript global PC_hz_ht to a non-zero form code), the body, head
        // and hair are displayed using the target race's parts, while equipment is still driven by
        // the player's real inventory. This lets armor and clothing be worn over the transformed
        // body - as the base race would - without the form appearance being lost. Creature forms are
        // handled separately by the mod (full actor replacement) and never set PC_hz_ht, so they are
        // unaffected.
        ESM::RefId mBodyRaceOverride; // empty = use the actor's real race
        bool mHazMangchu; // Mang'Chu form (its skin has no race, so it is resolved by explicit part ids)
        int mHazFormCode; // last-applied PC_hz_ht value for the player, -1 = not yet initialised
        int mHazHeadRoll; // random head-variant selector, re-rolled on each form change
        int mHazHairRoll; // random hair-variant selector, re-rolled independently of the head

        // Player only: read PC_hz_ht / PC_hazaeki and (re)apply the body override on change.
        void updateBodyRaceOverride();
        // Map a PC_hz_ht form code to a race id; returns an empty RefId for "no override".
        static ESM::RefId hazFormRace(int code);
        // First playable skin head (hair==false) or hair (hair==true) mesh for a race+sex, or empty.
        VFS::Path::Normalized findRaceHeadOrHair(const ESM::RefId& race, bool female, bool hair) const;
        // The race whose body/head/hair should be displayed (override if active, else the real race).
        ESM::RefId displayBodyRace() const;
        // Body parts for the Mang'Chu form, built by explicit id (its skin ships as clothing-tagged
        // parts rather than a race), indexed like getBodyParts()'s result. Empty if unavailable.
        std::vector<const ESM::BodyPart*> buildMangchuBodyParts(bool female) const;

        void updateNpcBase();

        NpcType getNpcType() const;

        PartHolderPtr insertBoundedPart(VFS::Path::NormalizedView model, std::string_view bonename,
            std::string_view bonefilter, bool enchantedGlow, osg::Vec4f* glowColor, bool isLight);

        void removeIndividualPart(ESM::PartReferenceType type);
        void reserveIndividualPart(ESM::PartReferenceType type, int group, int priority);

        void fitFirstPersonRobe(osg::Node& node);
        void fitFirstPersonWrists();
        bool addOrReplaceIndividualPart(ESM::PartReferenceType type, int group, int priority,
            VFS::Path::NormalizedView mesh, bool enchantedGlow = false, osg::Vec4f* glowColor = nullptr,
            bool isLight = false);
        void removePartGroup(int group);
        void addPartGroup(int group, int priority, const std::vector<ESM::PartReference>& parts,
            bool enchantedGlow = false, osg::Vec4f* glowColor = nullptr);

        VFS::Path::Normalized resolvePlayerEquipmentMesh(VFS::Path::NormalizedView normalMesh) const;
        VFS::Path::Normalized resolvePlayerNakedBodyMesh() const;

        void setRenderBin();

        osg::ref_ptr<RotateController> mFirstPersonNeckController;

        static bool isFemalePart(const ESM::BodyPart* bodypart);
        static NpcType getNpcType(const MWWorld::Ptr& ptr);

    protected:
        void addControllers() override;
        bool isArrowAttached() const override;
        std::string getSheathedShieldMesh(const MWWorld::ConstPtr& shield) const override;

    public:
        /**
         * @param ptr
         * @param disableListener  Don't listen for equipment changes and magic effects. InventoryStore only supports
         *                         one listener at a time, so you shouldn't do this if creating several NpcAnimations
         *                         for the same Ptr, eg preview dolls for the player.
         *                         Those need to be manually rendered anyway.
         * @param disableSounds    Same as \a disableListener but for playing items sounds
         * @param viewMode
         */
        NpcAnimation(const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode,
            Resource::ResourceSystem* resourceSystem, bool disableSounds = false, ViewMode viewMode = VM_Normal,
            float firstPersonFieldOfView = 55.f, bool bodyAwareness = false);
        virtual ~NpcAnimation();

        void enableHeadAnimation(bool enable) override;
        int getPartForNodePath(const osg::NodePath& path) const;
        bool addBloodDecal(const osg::Vec3f& position, const osg::Vec3f& outward,
            std::string_view kind, bool power, int hitPart = -1);
        std::string getBloodDecalData() const;
        bool restoreBloodDecals(std::string_view data);
        std::map<std::string, double> getBloodDecalStats() const;
        void startCutBloodFlow();
        void startBisectionBloodFlow();
        void startLimbSeveringBloodFlow();
        struct SwordClinchGeometry
        {
            bool active = false, fitted = false, clamped = false;
            unsigned int frame = 0;
            int phase = 0;
            float elapsed = 0.f, sourceElapsed = 0.f, weight = 0.f, rawGap = 0.f, finalGap = 0.f, cameraError = 0.f;
            osg::Vec3f correction, requestedCorrection;
            osg::Vec3f playerHilt, playerAxis, opponentHilt, opponentAxis;
            osg::Vec3f playerRange, opponentRange, playerContact, opponentContact;
            float playerParameter = 0.f, opponentParameter = 0.f;
        };
        SwordClinchGeometry mSwordClinchGeometry;
        void restoreTransientPose() override;
        void fitSwordClinchPose(osg::NodeVisitor& visitor);
        bool canFitSwordClinchPose(NpcAnimation& opponent);
        const SwordClinchGeometry& getSwordClinchGeometryForAudit() const { return mSwordClinchGeometry; }
        void configureSwordWallSweep(std::uint64_t swing, std::string_view group, std::string_view key);
        bool sampleSwordWallContact(float duration, const osg::FrameStamp& frameStamp);
        bool consumeSwordWallContact(std::uint64_t swing, std::string& key);
        const SwordWallContact& getSwordWallContactForAudit() const { return mSwordWallContact; }
        void recordWeaponCycleForAudit(const WeaponCycleState& state) { mWeaponCycleState = state; }
        const WeaponCycleState& getWeaponCycleForAudit() const { return mWeaponCycleState; }
        void recordSwordWallSound(bool played)
        {
            mSwordWallContact.soundPlayed = played;
            if (played)
                ++mSwordWallContact.sounds;
        }
        std::map<std::string, osg::Vec3f> getShieldGeometryForAudit() const;
        struct WeaponGripGeometry
        {
            std::map<std::string, std::string> models;
            std::map<std::string, osg::Matrixf> roots;
            std::map<std::string, std::vector<osg::Vec3f>> triangles;
        };
        WeaponGripGeometry getWeaponGripGeometryForAudit() const;
        std::shared_ptr<const ESM::ReleasedEquipmentState> captureReleasedEquipment(int slot,
            const osg::Matrixf& itemRoot, float mass, float& maxWorldVertexError) const;
        bool getReleasedEquipmentOwnerIsLeft(int slot) const;
        RagdollExtremityVertices getRagdollExtremityVertices(
            UnsupportedRagdollExtremityBones* unsupportedBones = nullptr) const override;
        bool setHeadSevered(bool severed);
        bool setTorsoSevered(bool severed);
        bool setLimbsSevered(unsigned mask);
        unsigned getSeveredLimbsMask() const;
        bool canSeverLegs() const;
        std::map<std::string, double> getLimbSeveringStats() const;
        std::map<std::string, osg::Vec3f> getLimbSeveringSurfacesForAudit() const;
        bool isTorsoSevered() const { return bool(mBisection); }
        std::map<std::string, double> getBisectionStats() const;
        std::map<std::string, osg::Vec3f> getBisectionSurfacesForAudit() const;
        std::map<std::string, double> getDecapitationStats() const;
        std::map<std::string, osg::Vec3f> getDecapitationSurfacesForAudit() const;
        bool useFullBodyFirstPerson() const;
        /// Reapply a run-time animation option to this actor.
        void reloadBrutalAnimations();
        const NpcAnimation* getBodyAwarenessAnimation() const { return mBodyAwarenessAnimation.get(); }
        bool useBodyCrouchTransitions() const override;
        bool usePrimaryCrouchTransitions() const override;
        const osg::Vec3f& getBodyGroundUp() const { return mBodyGroundUp; }
        const osg::Vec3f& getBodyGroundAnchor() const { return mBodyGroundAnchor; }
        float getBodyGroundBlend() const { return mBodyGroundBlend; }
        float getBodyGroundLift() const { return mBodyGroundLift; }
        std::map<std::string, osg::Vec3f> getFootGeometryForAudit(const osg::Vec3f& normal) const;
        unsigned int getBodyAwarenessVisibilityMask() const
        { return mBodyAwarenessRoot ? mBodyAwarenessRoot->getNodeMask() : 0u; }
        bool isBodyAwarenessHiddenForClinch() const { return mBodyAwarenessHiddenForClinch; }
        void setFirstPersonGeometryVisibleForAudit(bool visible);

        /// 1: the first person meshes follow the camera's rotation completely
        /// 0: the first person meshes follow the camera with a reduced factor, so you can look down at your own hands
        void setAccurateAiming(bool enabled) override;

        void setWeaponGroup(const std::string& group, bool relativeDuration) override;

        osg::Vec3f runAnimation(float timepassed) override;

        /// A relative factor (0-1) that decides if and how much the skeleton should be pitched
        /// to indicate the facing orientation of the character.
        void setPitchFactor(float factor) override { mPitchFactor = factor; }

        bool getWeaponsShown() const override { return mShowWeapons; }
        void showWeapons(bool showWeapon) override;

        bool updateCarriedLeftVisible(const int weaptype) const override;
        bool getCarriedLeftShown() const override { return mShowCarriedLeft; }
        void showCarriedLeft(bool show) override;

        void attachArrow() override;
        void detachArrow() override;
        void releaseArrow(float attackStrength) override;

        osg::Group* getArrowBone() override;
        osg::Node* getWeaponNode() override;
        Resource::ResourceSystem* getResourceSystem() override;

        // WeaponAnimation
        void showWeapon(bool show) override { showWeapons(show); }

//## VR_PATCH BEGIN
// Overridden in vr-animation
        virtual void setViewMode(ViewMode viewMode);

        virtual void updateParts();
//## VR_PATCH END

        /// Rebuilds the NPC, updating their root model, animation sources, and equipment.
        void rebuild();

        /// Get the inventory slot that the given node path leads into, or -1 if not found.
        int getSlot(const osg::NodePath& path) const;

        void setVampire(bool vampire) override;

        /// Set a translation offset (in object root space) to apply to meshes when in first person mode.
        void setFirstPersonOffset(const osg::Vec3f& offset);

        void updatePtr(const MWWorld::Ptr& updated) override;

        /// Get a list of body parts that may be used by an NPC of given race and gender.
        /// @note This is a fixed size list, one list item for each ESM::PartReferenceType, may contain nullptr body
        /// parts.
        static const std::vector<const ESM::BodyPart*>& getBodyParts(
            const ESM::RefId& raceId, bool female, bool firstperson, bool werewolf);
    };

}

#endif
