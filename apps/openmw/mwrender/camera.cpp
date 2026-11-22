#include "camera.hpp"

#include <osg/Camera>
#include <cmath>

#include <components/misc/mathutil.hpp>
#include <components/settings/values.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/visitor.hpp>

#include "../mwmechanics/brutaloptions.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/statemanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"

#include "../mwmechanics/movement.hpp"

#include "../mwphysics/raycasting.hpp"

#include "npcanimation.hpp"

//## VR_PATCH BEGIN
#include <components/vr/session.hpp>
#include <components/vr/trackinglistener.hpp>
#include <components/vr/trackingmanager.hpp>
#include <components/vr/viewer.hpp>
#include <components/vr/vr.hpp>
#include "../mwvr/openxrinput.hpp"
#include "../mwvr/vranimation.hpp"
//## VR_PATCH END

namespace
{

    class UpdateRenderCameraCallback : public SceneUtil::NodeCallback<UpdateRenderCameraCallback, osg::Camera*>
    {
    public:
        UpdateRenderCameraCallback(MWRender::Camera* cam)
            : mCamera(cam)
        {
        }

        void operator()(osg::Camera* cam, osg::NodeVisitor* nv)
        {
            // traverse first to update animations, in case the camera is attached to an animated node
            traverse(cam, nv);

            mCamera->updateCamera(cam);
        }

    private:
        MWRender::Camera* mCamera;
    };

}

namespace MWRender
{
    Camera::Camera(osg::Camera* camera)
        : mHeightScale(1.f)
        , mCollisionType(
              (MWPhysics::CollisionMask_Default & ~MWPhysics::Layers::ACTOR) | MWPhysics::Layers::CAMERA_ONLY)
        , mCamera(camera)
        , mAnimation(nullptr)
        , mFirstPersonView(true)
        , mMode(Mode::FirstPerson)
        , mVanityAllowed(true)
        , mDeferredRotationAllowed(true)
        , mProcessViewChange(false)
        , mHeight(124.f)
        , mPitch(0.f)
        , mYaw(0.f)
        , mRoll(0.f)
        , mCameraDistance(0.f)
        , mPreferredCameraDistance(0.f)
        , mFocalPointCurrentOffset(osg::Vec2d())
        , mFocalPointTargetOffset(osg::Vec2d())
        , mFocalPointTransitionSpeedCoef(1.f)
        , mSkipFocalPointTransition(true)
        , mPreviousTransitionInfluence(0.f)
        , mShowCrosshair(false)
        , mDeferredRotation(osg::Vec3f())
        , mDeferredRotationDisabled(false)
    {
        mUpdateCallback = new UpdateRenderCameraCallback(this);
//## VR_PATCH BEGIN
// VR-TODO: Instead of this convoluted method of forcing the desired order, why am i not
// just calling my callback from the regular update callback...
        if (mCamera->getUpdateCallback())
        {
            // Have to make sure the camera's callback is always called first
            std::vector<osg::ref_ptr<osg::Callback>> cbs;
            while (auto* cb = mCamera->getUpdateCallback())
            {
                cbs.push_back(cb);
                mCamera->removeUpdateCallback(cb);
            }
            mCamera->addUpdateCallback(mUpdateCallback);
            for (auto& cb : cbs)
                mCamera->addUpdateCallback(cb);
        }
        else
        {
            mCamera->addUpdateCallback(mUpdateCallback);
        }

        if (VR::getVR())
        {
            mMode = Mode::VR;
            processViewChange();
        }
//## VR_PATCH END
    }

    Camera::~Camera()
    {
        mCamera->removeUpdateCallback(mUpdateCallback);
    }

//## VR_PATCH BEGIN
    void Camera::updateTrackedPosition() const
    {
        if (!mTrackingNode)
            return;
        osg::NodePathList nodepaths = mTrackingNode->getParentalNodePaths();
        if (nodepaths.empty())
            return;
        mTrackedWorldMatrix = osg::computeLocalToWorld(nodepaths[0]);
        osg::Vec3d trans = mTrackedWorldMatrix.getTrans();
        const bool fullBody = mMode == Mode::FirstPerson && mAnimation && mAnimation->useFullBodyFirstPerson();
        if ((mMode != Mode::FirstPerson && mMode != Mode::VR) || fullBody)
            trans.z() += mHeight * mHeightScale;
        if (fullBody)
            trans.z() -= mFullBodySneakOffset;
        mTrackedWorldMatrix.setTrans(trans);
    }

    void Camera::updateCamera()
    {
        updateCamera(mCamera);
    }

    void Camera::setPose(const Stereo::Pose& pose)
    {
        mTrackedPose = pose;
        mPosition = mTrackedPose.position.asMWUnits();
        updateCamera();
    }

    void Camera::onSpaceUpdate() 
    {
        auto tp = MWVR::OpenXRInput::instance().getSpace(MWVR::OpenXRInput::DefaultReferenceSpaceView)->locateInWorld();
        if (!tp.status)
            setPose(mTrackedPose);
        else
            setPose(tp.pose);
    }
    // ## VR_PATCH END

    osg::Vec3d Camera::getFocalPointOffset() const
    {
        osg::Vec3d offset;
        offset.x() = mFocalPointCurrentOffset.x() * cos(mYaw);
        offset.y() = mFocalPointCurrentOffset.x() * sin(mYaw);
        offset.z() = mFocalPointCurrentOffset.y();
        return offset;
    }

//## VR_PATCH BEGIN
// Include VR pose in position/orientation
    void Camera::getOrientation(osg::Quat& orientation) const
    {
        if (mMode == Mode::VR)
        {
            orientation = mTrackedPose.orientation;
        }
        else
        {
            orientation = osg::Quat(mRoll + mExtraRoll, osg::Vec3d(0, 1, 0))
                * osg::Quat(mPitch + mExtraPitch, osg::Vec3d(1, 0, 0))
                * osg::Quat(mYaw + mExtraYaw, osg::Vec3d(0, 0, 1));
            ;
        }
    }

    void Camera::getPosition(osg::Vec3d& position) const
    {

        position = mPosition;
        if (mMode == Mode::FirstPerson)
        {
            // It is a hack. Camera position depends on neck animation.
            // Animations are updated in OSG cull traversal and in order to avoid 1 frame delay we
            // recalculate the position here. Note that it becomes different from mPosition that
            // is used in other parts of the code.
            // TODO: detach camera from OSG animation and get rid of this hack.

            updateTrackedPosition();
            position = calculateFirstPersonPosition(mTrackedWorldMatrix.getTrans());
        }

        if (mMode == Mode::VR)
        {
            position = mTrackedPose.position.asMWUnits();
        }
    }

    void Camera::updateCamera(osg::Camera* cam)
    {
        osg::Vec3d pos;
        getPosition(pos);

        osg::Quat orient;
        getOrientation(orient);

        osg::Vec3d forward = orient * osg::Vec3d(0, 1, 0);
        osg::Vec3d up = orient * osg::Vec3d(0, 0, 1);
//## VR_PATCH END
        cam->setViewMatrixAsLookAt(pos, pos + forward, up);
        mViewMatrix = cam->getViewMatrix();
        mProjectionMatrix = cam->getProjectionMatrix();
    }

    void Camera::update(float duration, bool paused)
    {
        mLockPitch = mLockYaw = false;
        if (mQueuedMode && mAnimation->upperBodyReady())
            setMode(*mQueuedMode);
        if (mProcessViewChange)
            processViewChange();

        // only show the crosshair in game mode
        MWBase::WindowManager* wm = MWBase::Environment::get().getWindowManager();
        wm->showCrosshair(!wm->isGuiMode() && mShowCrosshair);

        if (!paused)
            updateFocalPointOffset(duration);
        updatePosition();
    }

    osg::Vec3d Camera::calculateFirstPersonPosition(const osg::Vec3d& trackedPosition) const
    {
        osg::Vec3d res = trackedPosition;
        const bool fullBody = mAnimation && mAnimation->useFullBodyFirstPerson();
        const float forward = fullBody ? Settings::game().mFirstPersonBodyForward.get() * mHeightScale : 0.f;
        osg::Vec2f horizontalOffset
            = Misc::rotateVec2f(osg::Vec2f(mFirstPersonOffset.x(), mFirstPersonOffset.y() + forward), mYaw);
        res.x() += horizontalOffset.x();
        res.y() += horizontalOffset.y();
        res.z() += mFirstPersonOffset.z();
        if (fullBody && (res - trackedPosition).length2() > 0.001)
        {
            // Keep the eye outside the chest without letting it look through nearby walls.
            const auto* rays = MWBase::Environment::get().getWorld()->getRayCasting();
            const auto hit = rays->castSphere(trackedPosition, res, 2.f, mCollisionType);
            if (hit.mHit)
            {
                const osg::Vec3d delta = res - trackedPosition;
                const osg::Vec3d safe = osg::Vec3d(hit.mHitPos + hit.mHitNormal * 2.f) - trackedPosition;
                res = trackedPosition + delta * std::clamp((safe * delta) / delta.length2(), 0.0, 1.0);
            }
        }
        return res;
    }

    void Camera::updatePosition()
    {
//## VR_PATCH BEGIN
        updateTrackedPosition();
        mTrackedPosition = mTrackedWorldMatrix.getTrans();
//## VR_PATCH END
        if (mMode == Mode::Static)
            return;
        if (mMode == Mode::FirstPerson)
        {
            mPosition = calculateFirstPersonPosition(mTrackedPosition);
            mCameraDistance = 0;
            return;
        }
//## VR_PATCH BEGIN
        if (mMode == Mode::VR)
        {
            return;
        }
//## VR_PATCH END

        constexpr float cameraObstacleLimit = 5.0f;
        constexpr float focalObstacleLimit = 10.f;

        const auto* rayCasting = MWBase::Environment::get().getWorld()->getRayCasting();

        // Adjust focal point to prevent clipping.
        osg::Vec3d focalOffset = getFocalPointOffset();
        osg::Vec3d focal = mTrackedPosition + focalOffset;
        focalOffset.z() += 10.f; // Needed to avoid camera clipping through the ceiling because
                                 // character's head can be a bit higher than the collision area.
        double offsetLen = focalOffset.length();
        if (offsetLen > 0)
        {
            MWPhysics::RayCastingResult result
                = rayCasting->castSphere(focal - focalOffset, focal, focalObstacleLimit, mCollisionType);
            if (result.mHit)
            {
                double adjustmentCoef
                    = -(result.mHitPos + result.mHitNormal * focalObstacleLimit - focal).length() / offsetLen;
                focal += focalOffset * std::max(-1.0, adjustmentCoef);
            }
        }

        // Adjust camera distance.
        mCameraDistance = mPreferredCameraDistance;
        osg::Quat orient
            = osg::Quat(mPitch + mExtraPitch, osg::Vec3d(1, 0, 0)) * osg::Quat(mYaw + mExtraYaw, osg::Vec3d(0, 0, 1));
        osg::Vec3d offset = orient * osg::Vec3d(0.f, -mCameraDistance, 0.f);
        MWPhysics::RayCastingResult result
            = rayCasting->castSphere(focal, focal + offset, cameraObstacleLimit, mCollisionType);
        if (result.mHit)
        {
            mCameraDistance = (result.mHitPos + result.mHitNormal * cameraObstacleLimit - focal).length();
            offset = orient * osg::Vec3d(0.f, -mCameraDistance, 0.f);
        }

        mPosition = focal + offset;
    }

    osg::Quat Camera::getOrient() const
    {
        return osg::Quat(mRoll + mExtraRoll, osg::Vec3d(0, 1, 0)) * osg::Quat(mPitch + mExtraPitch, osg::Vec3d(1, 0, 0))
            * osg::Quat(mYaw + mExtraYaw, osg::Vec3d(0, 0, 1));
    }

    void Camera::setMode(Mode newMode, bool force)
    {
        // Lua camera actions call setMode directly, bypassing toggleViewMode.
        // Static remains available for explicit scripted cameras.
        if ((Settings::game().mFirstPersonFullBody || MWMechanics::brutalDmFirstPerson()) && !Settings::game().mFirstPersonBodyAllowThirdPerson
            && (newMode == Mode::ThirdPerson || newMode == Mode::Preview || newMode == Mode::Vanity))
            newMode = Mode::FirstPerson;
        if (mMode == newMode)
        {
            mQueuedMode = std::nullopt;
            return;
        }
//## VR_PATCH BEGIN
        if (newMode != Mode::VR && VR::getVR())
            return;
//## VR_PATCH END
        Mode oldMode = mMode;
        if (!force && (newMode == Mode::FirstPerson || oldMode == Mode::FirstPerson) && mAnimation
            && !mAnimation->upperBodyReady())
        {
            // Changing the view will stop all playing animations, so if we are playing
            // anything important, queue the view change for later
            mQueuedMode = newMode;
            return;
        }
        mMode = newMode;
        mQueuedMode = std::nullopt;
//## VR_PATCH BEGIN
        if (newMode == Mode::FirstPerson || newMode == Mode::VR)
            mFirstPersonView = true;
        else if (newMode == Mode::ThirdPerson)
        {
            mFirstPersonView = false;
            calculateDeferredRotation();
        }
//## VR_PATCH END
        if (oldMode == Mode::FirstPerson || newMode == Mode::FirstPerson)
        {
            instantTransition();
            mProcessViewChange = true;
        }
    }

    void Camera::setFocalPointTargetOffset(const osg::Vec2d& v)
    {
        mFocalPointTargetOffset = v;
        mPreviousTransitionSpeed = mFocalPointTransitionSpeed;
        mPreviousTransitionInfluence = 1.0f;
    }

    void Camera::updateFocalPointOffset(float duration)
    {
        if (duration <= 0)
            return;

        if (mSkipFocalPointTransition)
        {
            mSkipFocalPointTransition = false;
            mPreviousExtraOffset = osg::Vec2d();
            mPreviousTransitionInfluence = 0.f;
            mFocalPointCurrentOffset = mFocalPointTargetOffset;
        }

        osg::Vec2d oldOffset = mFocalPointCurrentOffset;

        if (mPreviousTransitionInfluence > 0)
        {
            mFocalPointCurrentOffset -= mPreviousExtraOffset;
            mPreviousExtraOffset
                = mPreviousExtraOffset / mPreviousTransitionInfluence + mPreviousTransitionSpeed * duration;
            mPreviousTransitionInfluence
                = std::max(0.f, mPreviousTransitionInfluence - duration * mFocalPointTransitionSpeedCoef);
            mPreviousExtraOffset *= mPreviousTransitionInfluence;
            mFocalPointCurrentOffset += mPreviousExtraOffset;
        }

        osg::Vec2d delta = mFocalPointTargetOffset - mFocalPointCurrentOffset;
        if (delta.length2() > 0)
        {
            double coef = duration * (1.0 + 5.0 / delta.length()) * mFocalPointTransitionSpeedCoef
                * (1.0f - mPreviousTransitionInfluence);
            mFocalPointCurrentOffset += delta * std::min(coef, 1.0);
        }
        else
        {
            mPreviousExtraOffset = osg::Vec2d();
            mPreviousTransitionInfluence = 0.f;
        }

        mFocalPointTransitionSpeed = (mFocalPointCurrentOffset - oldOffset) / duration;
    }

    void Camera::toggleViewMode(bool force)
    {
//## VR_PATCH BEGIN
        if (mMode == Mode::VR)
            return;
//## VR_PATCH END
        if ((Settings::game().mFirstPersonFullBody || MWMechanics::brutalDmFirstPerson()) && !Settings::game().mFirstPersonBodyAllowThirdPerson)
            return;
        setMode(mFirstPersonView ? Mode::ThirdPerson : Mode::FirstPerson, force);
    }

    bool Camera::toggleVanityMode(bool enable)
    {
//## VR_PATCH BEGIN
        if (mMode == Mode::VR)
            return false;
//## VR_PATCH END
        if (enable && (Settings::game().mFirstPersonFullBody || MWMechanics::brutalDmFirstPerson()) && !Settings::game().mFirstPersonBodyAllowThirdPerson)
            return false;
        if (!enable)
            setMode(mFirstPersonView ? Mode::FirstPerson : Mode::ThirdPerson, false);
        else if (mVanityAllowed)
            setMode(Mode::Vanity, false);
        return (mMode == Mode::Vanity) == enable;
    }

    void Camera::setSneakOffset(float offset, float slideBlend)
    {
        mSneakTarget = offset;
        const auto world = MWBase::Environment::get().getWorld();
        // Use the same clock as NIF body fades: menus freeze both visuals, and
        // slow time cannot let the camera finish while the pose is still moving.
        const double now = world->getTimeManager()->getRenderingSimulationTime();
        const float duration = static_cast<float>(std::clamp(now - mSneakVisualTime, 0., .2));
        mSneakVisualTime = now;
        const bool smooth = mAnimation && mMode == Mode::FirstPerson && !mAnimation->useFullBodyFirstPerson()
            && MWMechanics::brutalDmFirstPerson() && slideBlend == 0.f
            && mTrackingPtr == world->getPlayerPtr()
            && mTrackingPtr.getClass().getCreatureStats(mTrackingPtr).mBrutalSlidePhase == 0
            && !mTrackingPtr.getClass().getCreatureStats(mTrackingPtr).mBrutalSlideBlocked;
        if (!smooth || !mVisualSneakInitialized)
        {
            mVisualSneakOffset = offset;
            mVisualSneakVelocity = 0.f;
            mVisualSneakInitialized = true;
        }
        else if (duration > 0.f)
        {
            // Exact critically damped response. Reversing the requested stance keeps
            // the visible position and velocity; actor stance/collision change immediately.
            constexpr float omega = 22.f;
            const float delta = mVisualSneakOffset - offset;
            const float decay = std::exp(-omega * duration);
            const float step = (mVisualSneakVelocity + omega * delta) * duration;
            mVisualSneakOffset = offset + (delta + step) * decay;
            mVisualSneakVelocity = (mVisualSneakVelocity - omega * step) * decay;
            if (std::abs(mVisualSneakOffset - offset) < .0001f && std::abs(mVisualSneakVelocity) < .001f)
            {
                mVisualSneakOffset = offset;
                mVisualSneakVelocity = 0.f;
            }
        }
        // The authored slide eye already has its own blend. Preserve it exactly.
        mFullBodySneakOffset = mVisualSneakOffset * (1.f - slideBlend) + 70.f * slideBlend;
        if (mAnimation)
            mAnimation->setFirstPersonOffset(osg::Vec3f(0, 0, -mFullBodySneakOffset));
//## VR_PATCH BEGIN
        if (VR::getVR())
            VR::setSneakOffsetEnabled(offset != 0.f);
//## VR_PATCH END    }

    void Camera::setYaw(float angle, bool force)
    {
        if (!mLockYaw || force)
            mYaw = static_cast<float>(Misc::normalizeAngle(angle));
        if (force)
            mLockYaw = true;
    }

    void Camera::setPitch(float angle, bool force)
    {
        const float epsilon = 0.000001f;
        float limit = static_cast<float>(osg::PI_2) - epsilon;
        if (!mLockPitch || force)
            mPitch = std::clamp(angle, -limit, limit);
        if (force)
            mLockPitch = true;
    }

    void Camera::setStaticPosition(const osg::Vec3d& pos)
    {
        if (mMode != Mode::Static)
            throw std::runtime_error("setStaticPosition can be used only if camera is in Static mode");
        mPosition = pos;
    }

    void Camera::setAnimation(NpcAnimation* anim)
    {
        mAnimation = anim;
        mProcessViewChange = true;
    }

    void Camera::processViewChange()
    {
//## VR_PATCH BEGIN
        if (mMode == Mode::VR)
        {
            //SceneUtil::FindByNameVisitor visitor("VrCamera");
            //if (mAnimation && mAnimation->getObjectRoot())
            //{
            //    mAnimation->getObjectRoot()->accept(visitor);
            //    mTrackingNode = visitor.mFoundNode;
            //}
            //mHeightScale = 1.f;
            mProcessViewChange = false;
            return;
        }

//## VR_PATCH END
        if (mTrackingPtr.isEmpty())
            return;
        if (mMode == Mode::FirstPerson)
        {
            mAnimation->setViewMode(NpcAnimation::VM_FirstPerson);
            if (mAnimation->useFullBodyFirstPerson())
            {
                // The real skeleton can run, kick and swing independently of the player's view.
                // Tracking the actor root avoids neck-animation bobbing and attack-induced camera jumps.
                SceneUtil::PositionAttitudeTransform* transform = mTrackingPtr.getRefData().getBaseNode();
                mTrackingNode = transform;
                mHeightScale = transform ? transform->getScale().z() : 1.f;
            }
            else
            {
                mTrackingNode = mAnimation->getNode("Camera");
                if (!mTrackingNode)
                    mTrackingNode = mAnimation->getNode("Head");
                mHeightScale = 1.f;
            }
        }
        else
        {
            mAnimation->setViewMode(NpcAnimation::VM_Normal);
            SceneUtil::PositionAttitudeTransform* transform = mTrackingPtr.getRefData().getBaseNode();
            mTrackingNode = transform;
            if (transform)
                mHeightScale = transform->getScale().z();
            else
                mHeightScale = 1.f;
        }
        mProcessViewChange = false;
    }

    void Camera::applyDeferredPreviewRotationToPlayer(float dt)
    {
        if (mMode != Mode::ThirdPerson || mTrackingPtr.isEmpty())
            return;

        osg::Vec3f rot = mDeferredRotation;
        float delta = rot.normalize();
        delta = std::min(delta, (delta + 1.f) * 3 * dt);
        rot *= delta;
        mDeferredRotation -= rot;

        if (mDeferredRotationDisabled)
        {
            mDeferredRotationDisabled = delta > 0.0001;
            rotateCameraToTrackingPtr();
            return;
        }

        auto& movement = mTrackingPtr.getClass().getMovementSettings(mTrackingPtr);
        movement.mRotation[0] += rot.x();
        movement.mRotation[1] += rot.y();
        movement.mRotation[2] += rot.z();
        if (std::abs(mDeferredRotation.z()) > 0.0001)
        {
            float s = std::sin(mDeferredRotation.z());
            float c = std::cos(mDeferredRotation.z());
            float x = movement.mPosition[0];
            float y = movement.mPosition[1];
            movement.mPosition[0] = x * c + y * s;
            movement.mPosition[1] = x * -s + y * c;
        }
    }

    void Camera::rotateCameraToTrackingPtr()
    {
//## VR_PATCH BEGIN
        if (mMode == Mode::Static || mMode == Mode::VR || mTrackingPtr.isEmpty())
//## VR_PATCH END
            return;
        setPitch(-mTrackingPtr.getRefData().getPosition().rot[0] - mDeferredRotation.x());
        setYaw(-mTrackingPtr.getRefData().getPosition().rot[2] - mDeferredRotation.z());
    }

    void Camera::instantTransition()
    {
        mVisualSneakOffset = mSneakTarget;
        mVisualSneakVelocity = 0.f;
        mVisualSneakInitialized = false;

        mSkipFocalPointTransition = true;
        mDeferredRotationDisabled = false;
        mDeferredRotation = osg::Vec3f();
        rotateCameraToTrackingPtr();
//## VR_PATCH BEGIN
        if(VR::getVR())
            VR::Session::instance().instantTransition();
//## VR_PATCH END
    }

    void Camera::calculateDeferredRotation()
    {
        if (mMode == Mode::Static)
        {
            mDeferredRotation = osg::Vec3f();
            return;
        }
        MWWorld::Ptr ptr = mTrackingPtr;
        if (mMode == Mode::Preview || mMode == Mode::Vanity || ptr.isEmpty())
            return;
//## VR_PATCH BEGIN
        if (mFirstPersonView || mMode == Mode::VR)
//## VR_PATCH END
        {
            instantTransition();
            return;
        }

        mDeferredRotation.x()
            = static_cast<float>(Misc::normalizeAngle(-ptr.getRefData().getPosition().rot[0] - mPitch));
        mDeferredRotation.z() = static_cast<float>(Misc::normalizeAngle(-ptr.getRefData().getPosition().rot[2] - mYaw));
        if (!mDeferredRotationAllowed)
            mDeferredRotationDisabled = true;
    }

}
