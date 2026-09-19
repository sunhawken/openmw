#include "jigglebonecontroller.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/settings/values.hpp>

#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include <algorithm>
#include <cmath>
#include <string>

namespace MWRender
{
    namespace
    {
        // dt is clamped regardless of settings, so a hitch/pause doesn't blow up the spring.
        constexpr double sMaxDeltaTime = 0.1;

        // Extra static world-space Z (up/down) nudge for this bone's rest position,
        // read from the live-tunable settings sliders (Physics tab) rather than baked
        // into the NIF - identified by bone name since a single controller instance
        // doesn't otherwise know if it's on a breast or butt bone.
        float manualZOffsetFor(const std::string& boneName)
        {
            // Case-insensitive: auto-rigged bones are named "Bip01 L Breast" while some
            // externally-rigged NIFs use lowercase "bip01 l breast".
            if (Misc::StringUtils::ciFind(boneName, "breast") != std::string::npos)
                return Settings::game().mJiggleBoneBreastZOffset;
            if (Misc::StringUtils::ciFind(boneName, "butt") != std::string::npos)
                return Settings::game().mJiggleBoneButtZOffset;
            return 0.f;
        }
    }

    JiggleBoneController::JiggleBoneController(bool debug, bool isPlayer)
        : mSimWorldPos(0, 0, 0)
        , mVelocity(0, 0, 0)
        , mInitialized(false)
        , mLastSimTime(-1.0)
        , mDebug(debug)
        , mIsPlayer(isPlayer)
    {
    }

    float JiggleBoneController::zOffsetFor(const std::string& boneName) const
    {
        // NPC bones drop the manual breast/butt Z offset only while "jiggle player only" is on -
        // that toggle also scopes the player-mesh-specific Z tuning to the player. This is read live,
        // so already-loaded NPCs stop using the offset the moment the toggle is enabled. When the
        // toggle is off, NPCs share the same offset as the player, as before. The player's own bones
        // always use it.
        if (!mIsPlayer && Settings::game().mJiggleBonePlayerOnly)
            return 0.f;
        return manualZOffsetFor(boneName);
    }

    void JiggleBoneController::operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv)
    {
        // getParentalNodePaths() includes `node` itself as the path's last element;
        // drop it so we get the PARENT's world transform, since the offset needs to
        // be computed in the parent's space, not the bone's own (about to be modified).
        osg::Matrix parentWorldMatrix;
        osg::NodePathList nodePaths = node->getParentalNodePaths();
        if (!nodePaths.empty() && nodePaths[0].size() > 1)
        {
            osg::NodePath parentPath = nodePaths[0];
            parentPath.pop_back();
            parentWorldMatrix = osg::computeLocalToWorld(parentPath);
        }

        const double simTime = nv->getFrameStamp() ? nv->getFrameStamp()->getSimulationTime() : 0.0;

        if (!mInitialized)
        {
            // Cache the true NIF bind-pose local matrix exactly once. We must not
            // re-read node->getMatrix() on later frames: this same function just
            // wrote the jiggled position into that matrix, so re-reading it would
            // mean "rest" drifts to wherever the spring last left off, collapsing
            // the restoring force to ~0 after the very first frame.
            mRestLocalMatrix = node->getMatrix();
            osg::Vec3f restWorldPos = mRestLocalMatrix.getTrans() * parentWorldMatrix;
            restWorldPos.z() += zOffsetFor(node->getName());
            mSimWorldPos = restWorldPos;
            mVelocity = osg::Vec3f(0, 0, 0);
            mInitialized = true;
            mLastSimTime = simTime;
            traverse(node, nv);
            return;
        }

        const osg::Vec3f restTranslation = mRestLocalMatrix.getTrans();
        osg::Vec3f restWorldPos = restTranslation * parentWorldMatrix;
        restWorldPos.z() += zOffsetFor(node->getName());

        // Live master off-switch: the "jiggle auto rig" toggle freezes spring motion, but must
        // leave the manual NIF bone mover active. This lets the Jiggle-tab breast slider position
        // an existing .nif breast bone live even when procedural auto-rigging is disabled.
        if (!Settings::game().mJiggleAutoRig)
        {
            mSimWorldPos = restWorldPos;
            mVelocity = osg::Vec3f(0, 0, 0);
            mLastSimTime = simTime;
            const osg::Vec3f newLocalTranslation = restWorldPos * osg::Matrix::inverse(parentWorldMatrix);
            if (auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(node))
                nifTransform->setTranslation(newLocalTranslation);
            else
            {
                osg::Matrix newMatrix = mRestLocalMatrix;
                newMatrix.setTrans(newLocalTranslation);
                node->setMatrix(newMatrix);
            }
            traverse(node, nv);
            return;
        }

        double dt = simTime - mLastSimTime;
        mLastSimTime = simTime;
        if (dt <= 0.0)
        {
            traverse(node, nv);
            return;
        }
        if (dt > sMaxDeltaTime)
            dt = sMaxDeltaTime;
        const float fdt = static_cast<float>(dt);

        const float baseStiffness = Settings::game().mJiggleBoneStiffness;
        const float baseDamping = Settings::game().mJiggleBoneDamping;
        const float maxDisplacement = Settings::game().mJiggleBoneMaxDisplacement;
        const float intensity = Settings::game().mJiggleBoneIntensity;

        // --- TittyMagic-style feel: softness / quickness / per-bone mass modulate the base
        // spring, and a real world-space gravity term makes the bone sag + swing. ---
        // Softness (0..1): softer -> looser spring and less damping (freer, bouncier, slower to
        // settle). Mirrors TittyMagic's breastSoftness driving spring & damper down together.
        const float softness = std::clamp(Settings::game().mJiggleBoneSoftness / 100.f, 0.f, 1.f);
        // Quickness (0..1): snappier -> stiffer spring so it reacts and returns faster
        // (TittyMagic's breastQuickness offsets the joint spring up/down around its base).
        const float quickness = std::clamp(Settings::game().mJiggleBoneQuickness / 100.f, 0.f, 1.f);
        const float massResponse = Settings::game().mJiggleBoneMassResponse;

        // Per-bone "mass" proxy standing in for the breast volume/mass TittyMagic measures. Two
        // size hints, whichever is larger: (1) how far the bone rests from its parent joint - real
        // for externally/pre-rigged NIFs whose breast/butt bone carries an offset; (2) the magnitude
        // of the user's manual Z bulge offset - the size hint that survives on auto-rigged bones,
        // which are injected as identity children of Spine2/Pelvis (rest offset ~0). Normalised
        // against a reference (~12 units) and capped, then scaled by mass-response. TittyMagic's
        // InvertMass: heavier -> softer spring, more damping (and thus more sag under gravity).
        const float restProtrusion = mRestLocalMatrix.getTrans().length();
        const float sizeHint = std::max(restProtrusion, std::abs(zOffsetFor(node->getName())));
        const float normalizedMass = std::clamp(sizeHint / 12.f, 0.f, 1.5f);
        const float massEffect = massResponse * normalizedMass;

        // lerp(1 -> 0.40) as softness rises; lerp(0.75 -> 1.6) as quickness rises; heavier softens.
        float stiffnessMult = (1.f - 0.60f * softness) * (0.75f + 0.85f * quickness) * (1.f - 0.30f * massEffect);
        // lerp(1 -> 0.55) as softness rises; heavier damps more.
        float dampingMult = (1.f - 0.45f * softness) * (1.f + 0.50f * massEffect);
        const float stiffness = baseStiffness * std::max(0.05f, stiffnessMult);
        const float damping = baseDamping * std::max(0.f, dampingMult);

        // Gravity as a real world-space acceleration (down = world -Z). Because the spring solves
        // in world space, the resulting droop stays world-down and, when converted back into the
        // (rotating) parent's local frame, becomes an orientation-dependent forward/back/side shift
        // - reproducing TittyMagic's whole up/down/forward/back/left-right gravity family from one
        // physical term. Heavier+softer bones sag more since equilibrium droop = gravity/stiffness.
        const float gravity = Settings::game().mJiggleBoneGravity;
        const osg::Vec3f gravityAccel(0.f, 0.f, -gravity);

        osg::Vec3f displacement = mSimWorldPos - restWorldPos;
        const osg::Vec3f acceleration = (displacement * -stiffness) + (mVelocity * -damping) + gravityAccel;
        mVelocity += acceleration * fdt;
        mSimWorldPos += mVelocity * fdt;

        // Self/body-collision containment: don't let the bone sink toward its parent joint (into
        // the torso) past the limit, and stop its inward velocity at that wall. Bone-space analog
        // of TittyMagic's soft self-collision + distance limit. Uses the outward radial from the
        // parent joint, so it needs no knowledge of the skeleton's axis convention.
        if (Settings::game().mJiggleBoneSelfCollision)
        {
            const float selfCollisionLimit = Settings::game().mJiggleBoneSelfCollisionLimit;
            const osg::Vec3f parentOriginWorld = parentWorldMatrix.getTrans();
            osg::Vec3f outwardWorld = restWorldPos - parentOriginWorld;
            const float outwardLen = outwardWorld.length();
            if (outwardLen > 1e-3f)
            {
                outwardWorld /= outwardLen;
                const osg::Vec3f dispW = mSimWorldPos - restWorldPos;
                const float inward = -(dispW * outwardWorld); // >0 when moving toward the body
                if (inward > selfCollisionLimit)
                {
                    mSimWorldPos += outwardWorld * (inward - selfCollisionLimit);
                    const float inwardVel = -(mVelocity * outwardWorld);
                    if (inwardVel > 0.f)
                        mVelocity += outwardWorld * inwardVel; // cancel the into-body velocity
                }
            }
        }

        displacement = mSimWorldPos - restWorldPos;
        if (displacement.length2() > maxDisplacement * maxDisplacement)
        {
            displacement.normalize();
            displacement *= maxDisplacement;
            mSimWorldPos = restWorldPos + displacement;
        }

        // "Side sway" scales the horizontal (world XY) part of the jiggle relative to the
        // vertical bounce, so side-to-side / forward-back motion can be emphasised or damped
        // independently of up-down. 1.0 = uniform (old behaviour); 0 = vertical bounce only.
        const float side = Settings::game().mJiggleBoneSide;
        const osg::Vec3f shapedDisplacement(displacement.x() * side, displacement.y() * side, displacement.z());
        const osg::Vec3f visualWorldPos = restWorldPos + shapedDisplacement * intensity;

        const osg::Matrix parentWorldInverse = osg::Matrix::inverse(parentWorldMatrix);
        const osg::Vec3f newLocalTranslation = visualWorldPos * parentWorldInverse;

        if (mDebug && (mDebugCounter++ % 60) == 0)
        {
            Log(Debug::Warning) << "Jiggle bone debug: node=" << node->getName() << " dt=" << dt
                                 << " restWorldPos=" << restWorldPos.x() << "," << restWorldPos.y() << ","
                                 << restWorldPos.z() << " simWorldPos=" << mSimWorldPos.x() << "," << mSimWorldPos.y()
                                 << "," << mSimWorldPos.z() << " displacementLen=" << displacement.length()
                                 << " newLocalTranslation=" << newLocalTranslation.x() << "," << newLocalTranslation.y()
                                 << "," << newLocalTranslation.z();
        }

        if (auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(node))
            nifTransform->setTranslation(newLocalTranslation);
        else
        {
            osg::Matrix newMatrix = mRestLocalMatrix;
            newMatrix.setTrans(newLocalTranslation);
            node->setMatrix(newMatrix);
        }

        traverse(node, nv);
    }
}
