#include "openxrinput.hpp"
#include "vranimation.hpp"
#include "vrgui.hpp"
#include "vrpointer.hpp"

#include <algorithm>
#include <cmath>

#include <osg/BlendFunc>
#include <osg/CullFace>
#include <osg/FrontFace>
#include <osg/Depth>
#include <osg/Drawable>
#include <osg/Fog>
#include <osg/LightModel>
#include <osg/MatrixTransform>
#include <osg/Object>
#include <osg/PolygonMode>
#include <osg/PositionAttitudeTransform>

#include <components/debug/debuglog.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <components/settings/settings.hpp>
#include <components/settings/values.hpp>

#include <components/esm3/loadrace.hpp>
#include <components/esm3/loadench.hpp>

#include <components/misc/constants.hpp>
#include <components/misc/mathutil.hpp>
#include <components/misc/twoboneik.hpp>

#include <components/vr/session.hpp>
#include <components/vr/trackingmanager.hpp>
#include <components/vr/trackingtransform.hpp>
#include <components/vr/vr.hpp>
#include <components/xr/session.hpp>

#include "../mwworld/esmstore.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/weapontype.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwrender/camera.hpp"
#include "../mwrender/renderingmanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/player.hpp"

#include "vrpointer.hpp"
#include "vrtracking.hpp"

namespace MWVR
{
    // Some weapon types, such as spellcast, are classified as melee even though they are not. At least not in the way i
    // want. All the false melee types have negative enum values, but also so does hand to hand. I think this covers all
    // cases
    static bool isMeleeWeapon(int type)
    {
        if (MWMechanics::getWeaponType(type)->mWeaponClass != ESM::WeaponType::Melee)
            return false;
        if (type == ESM::Weapon::HandToHand)
            return true;
        if (type >= 0)
            return true;

        return false;
    }

    /// Implements control of a finger by overriding rotation
    class FingerController : public osg::NodeCallback
    {
    public:
        FingerController() {}
        void setEnabled(bool enabled) { mEnabled = enabled; }
        void operator()(osg::Node* node, osg::NodeVisitor* nv);

    private:
        bool mEnabled = true;
    };

    void FingerController::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        if (!mEnabled)
        {
            traverse(node, nv);
            return;
        }

        // Since morrowinds assets do not do a particularly good job of imitating natural hands and poses,
        // the best i can do for pointing is to just make it point straight forward. While not
        // totally natural, it works.
        osg::Quat rotate{ 0, 0, 0, 1 };
        auto matrixTransform = node->asTransform()->asMatrixTransform();
        auto matrix = matrixTransform->getMatrix();
        matrix.setRotate(rotate);
        matrixTransform->setMatrix(matrix);

        // Omit nested callbacks to override animations of this node
        osg::ref_ptr<osg::Callback> ncb = getNestedCallback();
        setNestedCallback(nullptr);
        traverse(node, nv);
        setNestedCallback(ncb);
    }

    /// Implements control of a finger by overriding rotation
    class HandController : public osg::NodeCallback
    {
    public:
        HandController() = default;
        void setEnabled(bool enabled) { mEnabled = enabled; }
        void setFingerPointingMode(bool fingerPointingMode) { mFingerPointingMode = fingerPointingMode; }
        void operator()(osg::Node* node, osg::NodeVisitor* nv);

    private:
        bool mEnabled = true;
        bool mFingerPointingMode = false;
    };

    void HandController::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        if (!mEnabled)
        {
            traverse(node, nv);
            return;
        }
        float PI_2 = osg::PI_2f;
        if (node->getName() == "Bip01 L Hand")
            PI_2 = -PI_2;
        float PI_4 = PI_2 / 2.f;

        osg::Quat rotate{ 0, 0, 0, 1 };
        auto windowManager = MWBase::Environment::get().getWindowManager();
        auto weaponType = MWBase::Environment::get().getWorld()->getActiveWeaponType();
        // Morrowind models do not hold most weapons at a natural angle, so i rotate the hand
        // to more natural angles on weapons to allow more comfortable combat.
        if (!windowManager->isGuiMode() && !mFingerPointingMode)
        {

            switch (weaponType)
            {
                case ESM::Weapon::None:
                case ESM::Weapon::HandToHand:
                case ESM::Weapon::MarksmanThrown:
                case ESM::Weapon::Spell:
                case ESM::Weapon::Arrow:
                case ESM::Weapon::Bolt:
                    // No adjustment
                    break;
                case ESM::Weapon::MarksmanCrossbow:
                    // Crossbow points upwards. Assumedly because i am overriding hand animations.
                    rotate = osg::Quat(PI_4 / 1.05, osg::Vec3{ 0, 1, 0 }) * osg::Quat(0.06, osg::Vec3{ 0, 0, 1 });
                    break;
                case ESM::Weapon::MarksmanBow:
                    // Bow points down by default, rotate it back up a little
                    rotate = osg::Quat(-PI_2 * .10f, osg::Vec3{ 0, 1, 0 });
                    break;
                default:
                    // Melee weapons Need adjustment
                    rotate = osg::Quat(PI_4, osg::Vec3{ 0, 1, 0 });
                    break;
            }
        }

        auto matrixTransform = node->asTransform()->asMatrixTransform();
        auto matrix = matrixTransform->getMatrix();
        matrix.setRotate(rotate);
        matrixTransform->setMatrix(matrix);

        // Omit nested callbacks to override animations of this node
        osg::ref_ptr<osg::Callback> ncb = getNestedCallback();
        setNestedCallback(nullptr);
        traverse(node, nv);
        setNestedCallback(ncb);
    }

    class ConfigureCullVisitor : public osg::NodeVisitor
    {
    public:
        ConfigureCullVisitor(bool enable)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mEnable(enable)
        {
        }

        void apply(osg::Drawable& node) override { node.setCullingActive(mEnable); }
        void apply(osg::Geometry& node) override { node.setCullingActive(mEnable); }

        bool mEnable;
    };

    /// Implements control of weapon direction
    class WeaponDirectionController : public osg::NodeCallback
    {
    public:
        WeaponDirectionController() = default;
        void setEnabled(bool enabled) { mEnabled = enabled; }
        void operator()(osg::Node* node, osg::NodeVisitor* nv);

    private:
        bool mEnabled = true;
    };

    void WeaponDirectionController::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        if (!mEnabled)
        {
            traverse(node, nv);
            return;
        }

        // Arriving here implies a parent, no need to check
        auto parent = static_cast<osg::MatrixTransform*>(node->getParent(0));

        osg::Quat rotate{ 0, 0, 0, 1 };
        auto weaponType = MWBase::Environment::get().getWorld()->getActiveWeaponType();
        switch (weaponType)
        {
            case ESM::Weapon::MarksmanThrown:
            case ESM::Weapon::Spell:
            case ESM::Weapon::Arrow:
            case ESM::Weapon::Bolt:
            case ESM::Weapon::HandToHand:
            case ESM::Weapon::MarksmanBow:
            case ESM::Weapon::MarksmanCrossbow:
                // Rotate to point straight forward, reverting any rotation of the hand to keep aim consistent.
                rotate = parent->getInverseMatrix().getRotate();
                rotate = osg::Quat(-osg::PI_2, osg::Vec3{ 0, 0, 1 }) * rotate;
                break;
            default:
                // Melee weapons point straight up from the hand
                rotate = osg::Quat(osg::PI_2, osg::Vec3{ 1, 0, 0 });
                break;
        }

        auto matrixTransform = node->asTransform()->asMatrixTransform();
        auto matrix = matrixTransform->getMatrix();
        matrix.setRotate(rotate);
        matrixTransform->setMatrix(matrix);

        traverse(node, nv);
    }

    /// Implements control of the weapon pointer
    class WeaponPointerController : public osg::NodeCallback
    {
    public:
        WeaponPointerController() = default;
        void setEnabled(bool enabled) { mEnabled = enabled; }
        void operator()(osg::Node* node, osg::NodeVisitor* nv);

    private:
        bool mEnabled = true;
    };

    void WeaponPointerController::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        if (!mEnabled)
        {
            traverse(node, nv);
            return;
        }

        auto matrixTransform = node->asTransform()->asMatrixTransform();
        auto world = MWBase::Environment::get().getWorld();
        auto weaponType = world->getActiveWeaponType();
        auto windowManager = MWBase::Environment::get().getWindowManager();

        if (!isMeleeWeapon(weaponType) && !windowManager->isGuiMode())
        {
            // Ranged weapons should show a pointer to where they are targeting
            matrixTransform->setMatrix(osg::Matrix::scale(1.f, 64.f, 1.f));
        }
        else
        {
            matrixTransform->setMatrix(osg::Matrix::scale(1.f, 64.f, 1.f));
        }

        // First, update the base of the finger to the overriding orientation

        traverse(node, nv);
    }

    /// Drives one arm from a VR controller with a two-bone IK solve: shoulder -> elbow -> wrist.
    ///
    /// The tracked controller gives the wrist position and the hand's orientation. The upper arm turns about the
    /// shoulder so the elbow sits where the IK solve puts it (bending down and out, away from the body), the forearm
    /// then aims from that elbow at the wrist, and the hand gets a correction so its world orientation is exactly the
    /// tracked one. Every bone keeps its own length: a controller held farther away than the arm reaches leaves the
    /// arm straight and pointing at it instead of stretching the skin (the old forearm-only tracking moved the
    /// forearm away from the animated upper arm, smearing the elbow).
    ///
    /// Runs after the update traversal (animation, HandController, jiggle), so it overrides this frame's animated
    /// arm pose and the next frame starts from the animation again: nothing accumulates.
    class ArmIKController
    {
    public:
        ArmIKController(std::shared_ptr<VR::Space> space, osg::Vec3 baseOffset, bool left, bool mirror)
            : mSpace(space)
            , mBaseOffset(baseOffset)
            , mBaseOrientation(osg::PI_2, osg::Vec3f(0, 0, 1))
            , mLeft(left)
            , mMirror(mirror)
        {
            if (left)
                mBaseOrientation = osg::Quat(osg::PI, osg::Vec3f(1, 0, 0)) * mBaseOrientation;
        }

        /// @param bodyYaw the character's yaw (rot[2]), for the direction the elbow bends
        void update(float bodyYaw)
        {
            if (!mTransform || !mUpperArm || mTransform->getNumChildren() == 0)
                return;
            auto* handTransform = mTransform->getChild(0)->asTransform();
            osg::MatrixTransform* hand = handTransform ? handTransform->asMatrixTransform() : nullptr;
            if (!hand || mUpperArm->getParentalNodePaths().empty() || mTransform->getParentalNodePaths().empty())
                return;

            auto tp = mSpace->locateInWorld();
            if (!tp.status)
                return;

            // The tracked hand: its orientation, and where the wrist (the hand bone's origin) goes. The base offset
            // centres the hand mesh on the tracked point (an estimate from trial and error).
            const osg::Quat trackedRotation = mBaseOrientation * tp.pose.orientation;
            osg::Vec3f wrist = tp.pose.position.asMWUnits() - trackedRotation * mBaseOffset;
            osg::Vec3f handsOffset = VR::Session::instance().getHandsOffset();
            if (mLeft)
                handsOffset.x() = -handsOffset.x();
            wrist += tp.pose.orientation * handsOffset;

            // The arm as animated this frame.
            const osg::Matrix upperWorld = osg::computeLocalToWorld(mUpperArm->getParentalNodePaths()[0]);
            const osg::Matrix forearmWorld = osg::computeLocalToWorld(mTransform->getParentalNodePaths()[0]);
            const osg::Vec3f shoulder = upperWorld.getTrans();
            const osg::Vec3f elbowNow = forearmWorld.getTrans();
            const osg::Vec3f wristNow = hand->getMatrix().getTrans() * forearmWorld;
            const float upperLength = (elbowNow - shoulder).length();
            const float foreLength = (wristNow - elbowNow).length();
            if (upperLength < 1.f || foreLength < 1.f)
                return;

            // Never stretch: keep the wrist within the arm's reach (and out of the shoulder).
            osg::Vec3f toWrist = wrist - shoulder;
            float distance = toWrist.length();
            const float maxReach = (upperLength + foreLength) * 0.995f;
            const float minReach = std::max(std::abs(upperLength - foreLength), 0.3f * (upperLength + foreLength));
            if (distance < 1e-3f)
            {
                toWrist = osg::Vec3f(0, 0, -1);
                distance = 1.f;
            }
            wrist = shoulder + toWrist * (std::clamp(distance, minReach, maxReach) / distance);

            // Elbows bend down, out to the side and a little back: a hint point in the character's frame.
            const osg::Vec3f forward(std::sin(bodyYaw), std::cos(bodyYaw), 0.f);
            const osg::Vec3f right(std::cos(bodyYaw), -std::sin(bodyYaw), 0.f);
            const osg::Vec3f outward = mLeft ? -right : right;
            const osg::Vec3f pole
                = shoulder + (osg::Vec3f(0, 0, -1.f) + outward * 0.6f - forward * 0.35f) * upperLength;
            const Misc::TwoBoneIKResult ik = Misc::solveTwoBoneIK(shoulder, upperLength, foreLength, wrist, pole);
            const osg::Vec3f elbow = ik.middle;
            if (!isFinite(elbow) || !isFinite(wrist))
                return;

            // Upper arm: turn about the shoulder so the elbow lands on the solved position.
            osg::Quat upperDelta;
            upperDelta.makeRotate(elbowNow - shoulder, elbow - shoulder);
            const osg::Matrix upperWorldNew = upperWorld * osg::Matrix::translate(-shoulder)
                * osg::Matrix::rotate(upperDelta) * osg::Matrix::translate(shoulder);

            // The forearm rides along with it, then aims at the wrist. Starting from the tracked orientation and
            // turning only as much as needed keeps the forearm's twist on the controller's roll.
            const osg::Matrix forearmWorldMoved = forearmWorld * osg::Matrix::inverse(upperWorld) * upperWorldNew;
            const osg::Vec3f handLocal = hand->getMatrix().getTrans();
            osg::Quat aim;
            aim.makeRotate(trackedRotation * handLocal, wrist - elbow);
            const osg::Quat forearmRotation = trackedRotation * aim;
            osg::Vec3f scale = forearmWorldMoved.getScale();
            if (mMirror)
                scale.y() = -scale.y();
            const osg::Matrix forearmWorldNew = osg::Matrix::scale(scale) * osg::Matrix::rotate(forearmRotation)
                * osg::Matrix::translate(elbow);

            const osg::Matrix upperLocal = upperWorldNew * osg::Matrix::inverse(upperWorld) * mUpperArm->getMatrix();
            const osg::Matrix forearmLocal
                = forearmWorldNew * osg::Matrix::inverse(forearmWorldMoved) * mTransform->getMatrix();
            if (!isFinite(upperLocal) || !isFinite(forearmLocal))
                return;
            mUpperArm->setMatrix(upperLocal);
            mTransform->setMatrix(forearmLocal);

            // Hand: HandController has set its weapon-grip rotation G this frame; with the forearm turned by `aim`,
            // G * trackedRotation * forearmRotation^-1 gives the hand exactly the tracked orientation G * tracked.
            osg::Matrix handMatrix = hand->getMatrix();
            handMatrix.setRotate(handMatrix.getRotate() * trackedRotation * forearmRotation.inverse());
            if (isFinite(handMatrix))
                hand->setMatrix(handMatrix);
        }

        void setTransform(osg::MatrixTransform* transform)
        {
            if (mTransform)
                mTransform->setCullingActive(true);
            mTransform = transform;
            if (mTransform)
                mTransform->setCullingActive(false);
        }

        void setUpperArm(osg::MatrixTransform* upperArm) { mUpperArm = upperArm; }

        std::shared_ptr<VR::Space> mSpace;
        osg::ref_ptr<osg::MatrixTransform> mTransform; // the forearm
        osg::ref_ptr<osg::MatrixTransform> mUpperArm;
        osg::Vec3 mBaseOffset;
        osg::Quat mBaseOrientation;
        bool mLeft;
        bool mMirror;

    private:
        static bool isFinite(const osg::Vec3f& v)
        {
            return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
        }

        static bool isFinite(const osg::Matrix& m)
        {
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    if (!std::isfinite(m(i, j)))
                        return false;
            return true;
        }
    };

    VRAnimation::VRAnimation(const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode,
        Resource::ResourceSystem* resourceSystem, bool disableSounds, osg::ref_ptr<osg::Group> sceneRoot)
        // Note that i let it construct as 3rd person and then later update it to VM_VRFirstPerson
        // when the character controller updates
        : MWRender::NpcAnimation(ptr, parentNode, resourceSystem, disableSounds, VM_Normal, 55.f)
        // The player model needs to be pushed back a little to make sure the player's view point is naturally
        // protruding Pushing the camera forward instead would produce an unnatural extra movement when rotating the
        // player model.
        , mModelOffset(new osg::MatrixTransform(osg::Matrix::translate(osg::Vec3(0, -15, 0))))
        , mCrosshairsEnabled(false)
        , mSceneRoot(sceneRoot)
    {
        // mReferenceSpace = XR::Session::instance().getReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL);
        mLeftHandPath = VR::stringToXrPath(VR::Paths::LEFT_HAND);
        mRightHandPath = VR::stringToXrPath(VR::Paths::RIGHT_HAND);

        mWeaponDirectionTransform = new osg::MatrixTransform();
        mWeaponDirectionTransform->setName("Weapon Direction");
        mWeaponDirectionTransform->setUpdateCallback(new WeaponDirectionController);

        mModelOffset->setName("ModelOffset");

        mWeaponPointerTransform = new osg::MatrixTransform();
        mWeaponPointerTransform->setMatrix(osg::Matrix::scale(0.f, 0.f, 0.f));
        mWeaponPointerTransform->setName("Weapon Pointer");
        mWeaponPointerTransform->setUpdateCallback(new WeaponPointerController);
        // mWeaponDirectionTransform->addChild(mWeaponPointerTransform);

        auto& xrInput = OpenXRInput::instance();

        // Morrowind's meshes do not point forward by default and need re-positioning and orientation.
        osg::Vec3 offset{ 15, 0, 0 };

        for (int i = 0; i < 2; i++)
        {
            XrPath path = i == 0 ? mLeftHandPath : mRightHandPath;
            auto& ctx = mVrControllers[path] = {};
            ctx.topLevelPath = path;
            if (VR::getLeftHandedMode())
                ctx.spaceName = i == 1 ? OpenXRInput::LeftHandAim : OpenXRInput::RightHandAim;
            else
                ctx.spaceName = i == 0 ? OpenXRInput::LeftHandAim : OpenXRInput::RightHandAim;
            ctx.forearmBone = i == 0 ? "bip01 l forearm" : "bip01 r forearm";
            ctx.upperArmBone = i == 0 ? "bip01 l upperarm" : "bip01 r upperarm";
            ctx.armController = std::make_unique<ArmIKController>(
                xrInput.getSpace(ctx.spaceName), offset, i == 0, VR::getLeftHandedMode());
            ctx.handBone = i == 0 ? "Bip01 L Hand" : "Bip01 R Hand";
            ctx.handController = new HandController;
            ctx.indexFingerBone[0] = i == 0 ? "bip01 l finger1" : "bip01 r finger1";
            ctx.indexFingerControllers[0] = new FingerController;
            ctx.indexFingerBone[1] = i == 0 ? "bip01 l finger2" : "bip01 r finger2";
            ctx.indexFingerControllers[1] = new FingerController;
        }
    }

    VRAnimation::~VRAnimation() {}

    void VRAnimation::setViewMode(NpcAnimation::ViewMode viewMode)
    {
        if (viewMode != VM_VRFirstPerson && viewMode != VM_VRNormal)
        {
            Log(Debug::Warning)
                << "Attempted to set view mode of VRAnimation to non-vr mode. Defaulted to VM_VRFirstPerson.";
            viewMode = VM_VRFirstPerson;
        }
        NpcAnimation::setViewMode(viewMode);
        return;
    }

    void VRAnimation::updateParts()
    {
        NpcAnimation::updateParts();

        if (mViewMode == VM_VRFirstPerson)
        {
            // Hide the head, hair and neck (and, transitively, any worn helmet, which occupies the same
            // PRT_Head part slot): the camera sits inside the head, so these meshes would otherwise
            // clip across the view. Everything else -- torso, upper arms, forearm/wrist/hand,
            // legs, feet -- stays visible for a full-body presence (matching how e.g. SkyrimVR's
            // VRIK handles first-person body visibility).
            //
            // The tracked arms are solved shoulder to wrist (ArmIKController), so the whole arm stays one
            // piece from the hand up to the chest.
            removeIndividualPart(ESM::PartReferenceType::PRT_Hair);
            removeIndividualPart(ESM::PartReferenceType::PRT_Head);
            // The neck sits right under the eyes too: looking down, it filled the view as a dome in front of the
            // chest. It belongs to the head as far as the player's own view is concerned.
            removeIndividualPart(ESM::PartReferenceType::PRT_Neck);
        }
        else
        {
            removeIndividualPart(ESM::PartReferenceType::PRT_LForearm);
            removeIndividualPart(ESM::PartReferenceType::PRT_LWrist);
            removeIndividualPart(ESM::PartReferenceType::PRT_RForearm);
            removeIndividualPart(ESM::PartReferenceType::PRT_RWrist);
        }

        updateCharHeight();
    }

    void VRAnimation::updateCrosshairs()
    {
        if (!mCrosshairsEnabled)
            return;

        mCrosshairAmmo->hide();
        mCrosshairSpell->hide();
        mCrosshairThrown->hide();

        if (MWBase::Environment::get().getWindowManager()->isGuiMode())
            return;

        if (VR::getKBMouseModeActive())
        {
        }

        if (isArrowAttached())
        {
            if (VR::getKBMouseModeActive())
            {
                mCrosshairAmmo->setParent(mKBMouseCrosshairTransform);
                mCrosshairAmmo->setOffset(100.f);
                mCrosshairAmmo->setStretch(200.f);
            }
            else
            {
                mCrosshairAmmo->setParent(getArrowBone());
                mCrosshairAmmo->setStretch(100.f);
                mCrosshairAmmo->setOffset(15.f);
            }
            mCrosshairAmmo->show();
        }
        else
        {
            mCrosshairAmmo->hide();
            mCrosshairAmmo->setParent(nullptr);
        }

        const MWWorld::Class& cls = mPtr.getClass();
        MWWorld::InventoryStore& inv = cls.getInventoryStore(mPtr);
        MWMechanics::CreatureStats& stats = cls.getCreatureStats(mPtr);

        mCrosshairSpell->hide();
        mCrosshairSpell->setParent(nullptr);
        mCrosshairThrown->hide();
        mCrosshairThrown->setParent(nullptr);

        if (stats.getDrawState() == MWMechanics::DrawState::Spell)
        {
            auto selectedSpell = MWBase::Environment::get().getWindowManager()->getSelectedSpell();
            ;
            auto world = MWBase::Environment::get().getWorld();
            bool isMagicItem = false;

            if (selectedSpell.empty())
            {
                if (inv.getSelectedEnchantItem() != inv.end())
                {
                    const MWWorld::Ptr& enchantItem = *inv.getSelectedEnchantItem();
                    selectedSpell = enchantItem.getClass().getEnchantment(enchantItem);
                    isMagicItem = true;
                }
            }

            const MWWorld::ESMStore& store = world->getStore();
            const ESM::EffectList* effectList = nullptr;
            if (isMagicItem)
            {
                const ESM::Enchantment* enchantment = store.get<ESM::Enchantment>().find(selectedSpell);
                if (enchantment)
                    effectList = &enchantment->mEffects;
            }
            else
            {
                const ESM::Spell* spell = store.get<ESM::Spell>().find(selectedSpell);
                if (spell)
                    effectList = &spell->mEffects;
            }

            int rangeType = ESM::RT_Self;
            if (effectList)
            {
                for (auto& effect : effectList->mList)
                {
                    rangeType = std::max(rangeType, effect.mData.mRange);
                }
            }

            if (rangeType > 0)
            {
                if (VR::getKBMouseModeActive())
                {
                    mCrosshairSpell->setParent(mKBMouseCrosshairTransform);
                    mCrosshairSpell->setOffset(100.f);
                    mCrosshairSpell->setStretch(200.f);
                }
                else
                {
                    mCrosshairSpell->setParent(mWeaponDirectionTransform);
                    if (rangeType == 1)
                        mCrosshairSpell->setStretch(25.f);
                    else if (rangeType == 2)
                        mCrosshairSpell->setStretch(100.f);
                    mCrosshairSpell->setOffset(15.f);
                }
                mCrosshairSpell->show();
            }
        }
        else if (stats.getDrawState() == MWMechanics::DrawState::Weapon)
        {
            // TODO: Should probably create an accessor for Slot_CarriedRight's WeaponType so this verbose code
            // doens't have to be repeated everywhere.
            MWWorld::ConstContainerStoreIterator weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
            if (weapon != inv.end() && weapon->getType() == ESM::Weapon::sRecordId)
            {
                int type = weapon->get<ESM::Weapon>()->mBase->mData.mType;
                ESM::WeaponType::Class weapclass = MWMechanics::getWeaponType(type)->mWeaponClass;
                if (weapclass == ESM::WeaponType::Thrown)
                {
                    if (VR::getKBMouseModeActive())
                    {
                        mCrosshairThrown->setParent(mKBMouseCrosshairTransform);
                        mCrosshairThrown->setOffset(100.f);
                        mCrosshairThrown->setStretch(200.f);
                    }
                    else
                    {
                        mCrosshairThrown->setParent(mWeaponDirectionTransform);
                        mCrosshairThrown->setStretch(100.f);
                        mCrosshairThrown->setOffset(15.f);
                    }
                    mCrosshairThrown->show();
                }
            }
        }
    }

    void VRAnimation::updateCharHeight()
    {
        // Compute an approximate character height (eye level)
        // auto playerPtr = MWMechanics::getPlayer();
        // const MWWorld::LiveCellRef<ESM::NPC>* ref = playerPtr.get<ESM::NPC>();
        // const ESM::Race* race
        //    = MWBase::Environment::get().getWorld()->getStore().get<ESM::Race>().find(ref->mBase->mRace);
        // bool isMale = ref->mBase->isMale();
        // float charHeightFactor = isMale ? race->mData.mMaleHeight : race->mData.mFemaleHeight;
        // auto charHeightBase
        //    = Stereo::Unit::fromMeters(Settings::Manager::getFloat("character base height", "VR Debug"));
        // auto charHeight = charHeightBase * charHeightFactor;
        // VR::Session::instance().setCharHeight(charHeight);
        // Log(Debug::Verbose) << "Calculated character height: " << charHeight.asMeters();

        // Use the actual animation to directly get the character's height
        auto root = mObjectRoot;
        auto head = getNode("Bip01 Head");
        if (root && head)
        {
            // The head geometry needs to exist to find a rough estimate of the eye line.
            // This assumes we're using the 3rd person animation, which should be the case.
            if (mPartPriorities[ESM::PRT_Head] < 1 && !mHeadModel.empty())
                addOrReplaceIndividualPart(ESM::PRT_Head, -1, 1, mHeadModel);
            if (head->computeBound().radius() > 1.f)
            {
                auto path = head->getParentalNodePaths(root)[0];
                auto m = osg::computeLocalToWorld(path);
                mCharHeight = static_cast<float>(m.getTrans().z()) + head->computeBound().radius();
            }
            removeIndividualPart(ESM::PRT_Head);
        }
    }

    void VRAnimation::updateSpace()
    {
        auto localRef = OpenXRInput::instance().getSpace(OpenXRInput::DefaultReferenceSpaceLocal);
        auto viewRef = OpenXRInput::instance().getSpace(OpenXRInput::DefaultReferenceSpaceView);
        auto viewPose = viewRef->locate(*localRef);
        if (!viewPose.status)
            return;
        auto pose = viewPose.pose;
        float newYaw = 0.f;
        float oldYaw = 0.f;
        float pitch = 0.f;
        float roll = 0.f;
        Misc::getEulerAngles(mHeadPoseInLocalSpace.orientation, oldYaw, pitch, roll);
        Misc::getEulerAngles(pose.orientation, newYaw, pitch, roll);

        // Room-scale locomotion: how far the headset physically moved this frame, expressed
        // relative to the direction it's currently facing (self-referential, so it doesn't need
        // to reason about any accumulated offset between tracking space and the game world).
        // Consumed by modifyMovement(), which adds it on top of stick/animation-driven movement.
        // Suppressed on a recenter frame below, since that's a calibration jump, not real
        // walking.
        if (Settings::vr().mPhysicalMovement)
        {
            osg::Vec3 rawDelta
                = pose.position.asMWUnits() - mHeadPoseInLocalSpace.position.asMWUnits();
            osg::Vec3 facingRelative = pose.orientation.inverse() * rawDelta;
            mPhysicalMovementDelta += osg::Vec2f(facingRelative.x(), facingRelative.y());
        }

        mHeadPoseInLocalSpace = pose;
        float characterYaw = mPtr.getRefData().getPosition().rot[2];
        float characterYawDiff = characterYaw - mCharacterYaw;

        mCharacterYaw = newYaw - oldYaw + characterYaw;
        auto world = MWBase::Environment::get().getWorld();
        world->rotateObject(mPtr, osg::Vec3f(pitch, 0.f, mCharacterYaw), MWBase::RotationFlag_none);

        if (mRecenter)
        {
            Log(Debug::Verbose) << "VRAnimation: Recenter( vertical=" << VR::getShouldRecenterZ() << ", horizontal=" << VR::getShouldRecenterXY() << ")";

            // A recenter jump is a calibration event, not real walking -- discard whatever the
            // physical-movement delta above just picked up from it.
            mPhysicalMovementDelta = osg::Vec2f(0.f, 0.f);

            mRecenter = false;
        }
        // else
        {
            // Something else rotated the character. Modify the local space pose accordingly.
            mCharLocalSpacePose.orientation
                = osg::Quat(characterYawDiff, osg::Vec3d(0, 0, -1)) * mCharLocalSpacePose.orientation;
        }
        updateLocalSpaceWorldPose();
        updateBodyLean();
        updateBodyOffset();

        for (auto& it : mVrControllers)
        {
            if (!it.second.enabled)
                continue;

            it.second.armController->update(mPtr.getRefData().getPosition().rot[2]);
        }
        if (mSkeleton)
        {
            mSkeleton->markBoneMatriceDirty();
        }

        osg::Vec2 offset = osg::Vec2(0, 0);
        if (Settings::vr().mHandDirectedMovement && !VR::getKBMouseModeActive())
        {
            // The baseline behavior is view directed movement. To make hand directed movement, simply add
            // the rotation from view to hand.
            // I compute this by locating the hand relative to view and computing yaw/pitch of that, using a direction vector
            // to ignore roll
            auto hand = VR::getLeftHandedMode() ? MWVR::OpenXRInput::RightHandAim : MWVR::OpenXRInput::LeftHandAim;
            if (auto space = MWVR::OpenXRInput::instance().getSpace(hand))
            {
                auto tp = space->locate(*VR::Session::instance().getReferenceSpace(VR::ReferenceSpace::View));
                if (!!tp.status)
                {
                    // Compute the directional vector
                    auto forward = osg::Vec3(0, 1, 0);
                    auto dir = tp.pose.orientation * forward;
                    // Simple trigonometry to compute yaw/pitch
                    offset = osg::Vec2(std::asin(-dir.z()), std::atan2(dir.x(), dir.y()));
                }
            }
        }
        VR::Session::instance().setMovementAngleOffset(offset);
    }

    void VRAnimation::updateBodyLean()
    {
        auto spine = getBoneByName("bip01 spine1");
        if (!spine)
            return;
        auto* spineTransform = spine->asTransform() ? spine->asTransform()->asMatrixTransform() : nullptr;
        if (!spineTransform || spineTransform->getNumChildren() == 0)
            return;
        auto* childTransform = spineTransform->getChild(0)->asTransform()
            ? spineTransform->getChild(0)->asTransform()->asMatrixTransform()
            : nullptr;
        if (!childTransform)
            return;

        // How far the head has physically moved, horizontally, from the local-space pose that
        // corresponds to the character's neutral/centered stance (maintained by
        // updateLocalSpaceWorldPose() above). This is the same signal used to reproject VR space
        // onto the game world, so it stays correct across recentering and character rotation.
        osg::Vec3 headLocal = mHeadPoseInLocalSpace.position.asMWUnits();
        osg::Vec3 neutralLocal = mCharLocalSpacePose.position.asMWUnits();
        osg::Vec2f lean(headLocal.x() - neutralLocal.x(), headLocal.y() - neutralLocal.y());
        constexpr float maxLean = 20.f;
        float leanLen = lean.length();
        if (!std::isfinite(leanLen))
            return;
        if (leanLen > maxLean)
            lean *= maxLean / leanLen;
        // Smoothed, so tracking jitter and recentre jumps don't shake the body.
        mSmoothedLean = mSmoothedLean * 0.8f + lean * 0.2f;
        lean = mSmoothedLean;

        // Translate the physical lean into a small torso tilt -- leaning forward pitches the
        // spine forward, leaning sideways rolls it -- applied as a delta rotation from the
        // spine's current (animated) direction toward its child, exactly like the arm IK above,
        // so no assumption about the bone's own rest-axis convention is needed.
        // At most about 10 degrees of tilt (20 units of lean): enough to follow the player, never a fold.
        constexpr float leanToTilt = 0.009f;
        osg::Matrix parentWorld = osg::computeLocalToWorld(spineTransform->getParentalNodePaths()[0]);
        osg::Matrix spineWorld = spineTransform->getMatrix() * parentWorld;
        osg::Matrix childWorld = childTransform->getMatrix() * spineWorld;

        osg::Vec3 spineOrigin = spineWorld.getTrans();
        osg::Vec3 oldDir = childWorld.getTrans() - spineOrigin;
        if (oldDir.length2() < 1e-6f)
            return;
        oldDir.normalize();

        osg::Vec3 newDir = oldDir + osg::Vec3(lean.x(), lean.y(), 0.f) * leanToTilt;
        newDir.normalize();

        osg::Quat delta;
        delta.makeRotate(oldDir, newDir);
        osg::Quat newWorldRotation = spineWorld.getRotate() * delta;

        osg::Matrix worldToParent = osg::Matrix::inverse(parentWorld);
        osg::Matrix newLocal = osg::Matrix::rotate(newWorldRotation) * osg::Matrix::translate(spineOrigin) * worldToParent;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (!std::isfinite(newLocal(i, j)))
                    return;
        spineTransform->setMatrix(newLocal);
    }

    void VRAnimation::updateBodyOffset()
    {
        // Manual calibration offset for the visible first-person body, same idea and same 0-1
        // scale as the hands offset sliders above (0.5 = centred/no offset). Applied to the root
        // bone so it shifts the whole body together. The underlying walk/idle animation drives
        // this bone's matrix fresh every frame before this runs (the same assumption
        // updateBodyLean() above relies on), so simply adding the offset to whatever's currently
        // there is safe and doesn't accumulate across frames.
        osg::Vec3 offset((Settings::vr().mBodyOffsetX - 0.5f) * Constants::UnitsPerMeter,
            (Settings::vr().mBodyOffsetY - 0.5f) * Constants::UnitsPerMeter,
            (Settings::vr().mBodyOffsetZ - 0.5f) * Constants::UnitsPerMeter);
        if (offset.length2() < 1e-6f)
            return;

        auto root = getBoneByName("bip01");
        if (!root)
            return;
        auto* rootTransform = root->asTransform() ? root->asTransform()->asMatrixTransform() : nullptr;
        if (!rootTransform)
            return;

        osg::Matrix matrix = rootTransform->getMatrix();
        matrix.setTrans(matrix.getTrans() + offset);
        rootTransform->setMatrix(matrix);
    }

    void VRAnimation::modifyMovement(osg::Vec3& movement)
    {
        // Room-scale locomotion: add however far the player physically walked this frame on top
        // of whatever stick/animation-driven movement was already decided above. Consumed once
        // and reset so it's never applied twice if this is called more than once per updateSpace()
        // tick.
        movement.x() += mPhysicalMovementDelta.x();
        movement.y() += mPhysicalMovementDelta.y();
        mPhysicalMovementDelta = osg::Vec2f(0.f, 0.f);
    }

    void VRAnimation::addControllers()
    {
        NpcAnimation::addControllers();
        updateTrackingControllers();
        mSkeleton->setIsTracked(true);

        ConfigureCullVisitor configureCullVisitor(false);
        mObjectRoot->accept(configureCullVisitor);

        osg::ref_ptr<osg::StateSet> stateSet = mObjectRoot->getOrCreateStateSet();
        stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    }

    void VRAnimation::updateTrackingControllers()
    {
        for (auto& it : mVrControllers)
        {
            disableTracking(it.second.topLevelPath);
            if (VR::getControllerActive(it.second.topLevelPath))
                enableTracking(it.second.topLevelPath);
        }

        auto hand = mNodeMap.find("Bip01 R Hand");
        if (hand != mNodeMap.end())
        {
            hand->second->removeChild(mWeaponDirectionTransform);
            hand->second->addChild(mWeaponDirectionTransform);
        }
    }

    void VRAnimation::enableTracking(XrPath path)
    {
        auto& ctx = mVrControllers[path];

        auto forearm = mNodeMap.find(ctx.forearmBone);
        if (forearm != mNodeMap.end())
        {
            ctx.armController->setTransform(forearm->second);
        }
        auto upperArm = mNodeMap.find(ctx.upperArmBone);
        ctx.armController->setUpperArm(upperArm != mNodeMap.end() ? upperArm->second.get() : nullptr);

        auto hand = mNodeMap.find(ctx.handBone);
        if (hand != mNodeMap.end())
        {
            auto node = hand->second;
            node->addUpdateCallback(ctx.handController);
        }

        auto finger1 = mNodeMap.find(ctx.indexFingerBone[0]);

        if (finger1 != mNodeMap.end())
        {
            auto& base_joint = finger1->second;
            auto second_joint = base_joint->getChild(0)->asTransform()->asMatrixTransform();

            base_joint->addUpdateCallback(ctx.indexFingerControllers[0]);
            if (second_joint)
                second_joint->addUpdateCallback(ctx.indexFingerControllers[1]);
        }

        ctx.enabled = true;
    }

    void VRAnimation::disableTracking(XrPath path) {
        auto& ctx = mVrControllers[path];
        ctx.armController->setTransform(nullptr);
        ctx.armController->setUpperArm(nullptr);
        auto forearm = mNodeMap.find(ctx.forearmBone);
        if (forearm != mNodeMap.end())
        {
            ConfigureCullVisitor configureCullVisitor(true);
            forearm->second->accept(configureCullVisitor);
        }

        auto hand = mNodeMap.find(ctx.handBone);
        if (hand != mNodeMap.end())
        {
            auto node = hand->second;
            node->removeUpdateCallback(ctx.handController);
        }

        auto finger1 = mNodeMap.find(ctx.indexFingerBone[0]);

        if (finger1 != mNodeMap.end())
        {
            auto& base_joint = finger1->second;
            auto second_joint = base_joint->getChild(0)->asTransform()->asMatrixTransform();

            base_joint->removeUpdateCallback(ctx.indexFingerControllers[0]);
            if (second_joint)
                second_joint->removeUpdateCallback(ctx.indexFingerControllers[1]);
        }
        ctx.enabled = false;
    }

    void VRAnimation::enablePointer(XrPath topLevelPath, bool enable) 
    {
        auto& ctx = mVrControllers[topLevelPath];
        ctx.handController->setFingerPointingMode(enable);
        ctx.indexFingerControllers[0]->setEnabled(enable);
        ctx.indexFingerControllers[1]->setEnabled(enable);
    }

    void VRAnimation::enableHeadAnimation(bool)
    {
        NpcAnimation::enableHeadAnimation(false);
    }

    void VRAnimation::setAccurateAiming(bool)
    {
        NpcAnimation::setAccurateAiming(false);
    }

    void VRAnimation::enablePointers(bool left, bool right)
    {
        if (VR::getLeftHandedMode())
            std::swap(left, right);
        enablePointer(mLeftHandPath, left);
        enablePointer(mRightHandPath, right);
    }

    void VRAnimation::setEnableCrosshairs(bool enable)
    {
        if (enable == mCrosshairsEnabled)
            return;

        mCrosshairsEnabled = enable;

        if (enable)
        {
            mCrosshairAmmo = std::make_unique<Crosshair>(nullptr, osg::Vec3f(0.66f, 1.f, 0.66f), 0.1f, 0.40f, false);
            mCrosshairAmmo->setStretch(100.f);
            mCrosshairAmmo->setWidth(0.1f);
            mCrosshairAmmo->setOffset(15.f);
            mCrosshairAmmo->show();
            mCrosshairThrown = std::make_unique<Crosshair>(nullptr, osg::Vec3f(0.66f, 0.66f, 1.f), 0.1f, 0.40f, false);
            mCrosshairThrown->setStretch(100.f);
            mCrosshairThrown->setWidth(0.1f);
            mCrosshairThrown->setOffset(15.f);
            mCrosshairThrown->show();
            mCrosshairSpell = std::make_unique<Crosshair>(nullptr, osg::Vec3f(1.f, 1.f, 1.f), 0.1f, 0.40f, false);
            mCrosshairSpell->setStretch(100.f);
            mCrosshairSpell->setWidth(0.1f);
            mCrosshairSpell->setOffset(15.f);
            mCrosshairSpell->show();

            mKBMouseCrosshairTransform = new VR::SpaceTransform(OpenXRInput::instance().getSpace(OpenXRInput::DefaultReferenceSpaceView));
            //mKBMouseCrosshairTransform->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
            mSceneRoot->addChild(mKBMouseCrosshairTransform);
        }
        else
        {
            mCrosshairAmmo = nullptr;
            mCrosshairThrown = nullptr;
            mCrosshairSpell = nullptr;

            mSceneRoot->removeChild(mKBMouseCrosshairTransform);
            mKBMouseCrosshairTransform = nullptr;
        }
    }

    void VRAnimation::updateLocalSpaceWorldPose()
    {
        auto origin = mObjectRoot->getParent(0);
        auto worldMatrix = osg::computeLocalToWorld(origin->getParentalNodePaths()[0]);
        auto trans = worldMatrix.getTrans();
        trans.z() += mCharHeight;
        Stereo::Pose originPose = { Stereo::Position::fromMWUnits(trans), osg::Quat{ 0, 0, 0, 1 } };
        XR::Session::instance().setReferenceWorldPose(originPose + mCharLocalSpacePose);
    }

    void VRAnimation::recenter()
    {
        mRecenter = true;
    }

    void VRAnimation::onInteractionProfileActiveChanged(XrPath topLevelPath, bool isActive) 
    {
        updateTrackingControllers();
    }

    void VRAnimation::addAnimSource(std::string_view model, const std::string& baseModel) 
    {
        Animation::addAnimSource(model, baseModel);
        updateCharHeight();
    }
}
