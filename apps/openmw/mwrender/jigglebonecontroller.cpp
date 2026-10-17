#include "jigglebonecontroller.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/jigglepolicy.hpp>
#include <components/misc/jigglezoffset.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/settings/values.hpp>

#include <osg/Math>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "jiggleglute.hpp"

namespace MWRender
{
    namespace
    {
        // dt is clamped regardless of settings, so a hitch/pause doesn't blow up the spring.
        constexpr double sMaxDeltaTime = 0.1;
        // A one-frame jump this large is a teleport/cell transition, not normal character motion.
        // Reset instead of feeding it into the spring and carrying a huge velocity into later frames.
        constexpr float sTeleportResetDistance = 128.f;
        // Cap on the body acceleration fed to the springs (game units/s^2).
        constexpr float sMaxRestAccel = 6000.f;

        // Lever arm (game units) used to turn the Naturalis glute angle offsets into a rest-position
        // shift - roughly the auto-rigger's butt weight-cone radius.
        constexpr float sGluteLever = 9.f;

        bool isLeftGlute(const std::string& boneName)
        {
            return Misc::StringUtils::ciFind(boneName, "l butt") != std::string::npos;
        }

        // World matrix of the actor's skeleton root. Skinned body meshes (and the auto-rigger's
        // anchors) live in this space: +X forward, +Y left, +Z up.
        std::optional<osg::Matrix> findSkeletonWorld(const osg::NodePath& path)
        {
            for (std::size_t i = path.size(); i-- > 0;)
            {
                if (dynamic_cast<SceneUtil::Skeleton*>(path[i]) != nullptr)
                {
                    osg::NodePath skeletonPath(path.begin(), path.begin() + i + 1);
                    return osg::computeLocalToWorld(skeletonPath);
                }
            }
            return std::nullopt;
        }

        // Static rest shift from Naturalis's glute Up/Down and Left/Right angle offsets.
        osg::Vec3f gluteRestOffset(const std::string& boneName, const std::optional<osg::Matrix>& skeletonWorld)
        {
            if (!skeletonWorld)
                return osg::Vec3f();
            const float upDown = osg::DegreesToRadians(Settings::game().mJiggleGluteUpDownAngle.get());
            const float leftRight = osg::DegreesToRadians(Settings::game().mJiggleGluteLeftRightAngle.get());
            // Positive left/right pushes the glutes together: toward -Y for the left glute, +Y for the right.
            const float inward = isLeftGlute(boneName) ? -1.f : 1.f;
            const osg::Vec3f local(0.f, inward * sGluteLever * std::sin(leftRight), sGluteLever * std::sin(upDown));
            return osg::Matrix::transform3x3(local, *skeletonWorld);
        }
    }

    JiggleBoneController::JiggleBoneController(bool debug, bool isPlayer, std::string actorName, ESM::FormId refNum)
        : mOffset(0, 0, 0)
        , mVelocity(0, 0, 0)
        , mPreviousRestWorldPos(0, 0, 0)
        , mInitialized(false)
        , mLastSimTime(-1.0)
        , mDebug(debug)
        , mIsPlayer(isPlayer)
        , mActorName(std::move(actorName))
        , mRefNum(refNum)
    {
    }

    float JiggleBoneController::zOffsetFor(const std::string& boneName)
    {
        if (mBoneKind == BoneKind::Unknown)
        {
            // Case-insensitive: auto-rigged bones are "Bip01 L Breast", some NIFs use lowercase.
            if (Misc::StringUtils::ciFind(boneName, "breast") != std::string::npos)
                mBoneKind = BoneKind::Breast;
            else if (Misc::StringUtils::ciFind(boneName, "butt") != std::string::npos)
                mBoneKind = BoneKind::Butt;
            else
                mBoneKind = BoneKind::Other;
        }
        if (mBoneKind == BoneKind::Other)
            return 0.f;

        const unsigned generation = Misc::JiggleZOffset::generation();
        if (generation != mZGeneration)
        {
            mZGeneration = generation;
            mStoredZ.reset();
            const std::string meshFile = Misc::JiggleZOffset::currentActorMesh(mIsPlayer, mActorName);
            if (!meshFile.empty())
                mStoredZ = Misc::JiggleZOffset::lookupForActor(mIsPlayer, mActorName, meshFile);
        }
        if (mStoredZ)
            return mBoneKind == BoneKind::Breast ? mStoredZ->first : mStoredZ->second;
        return mBoneKind == BoneKind::Breast ? Settings::game().mJiggleBoneBreastZOffset.get()
                                             : Settings::game().mJiggleBoneButtZOffset.get();
    }

    void JiggleBoneController::operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv)
    {
        // The offset is computed in the PARENT's space, not the bone's own (about to be modified).
        // The visitor's current path ends with `node`; drop it, reusing one buffer, instead of
        // node->getParentalNodePaths(), which allocated a list of paths every frame.
        osg::Matrix parentWorldMatrix;
        const osg::NodePath& nodePath = nv->getNodePath();
        if (nodePath.size() > 1)
        {
            mParentPath.assign(nodePath.begin(), nodePath.end() - 1);
            parentWorldMatrix = osg::computeLocalToWorld(mParentPath);
        }

        // Naturalis-style glute physics applies to butt bones of body Jiggle.
        zOffsetFor(node->getName()); // classifies the bone on the first frame
        const bool glute = mBoneKind == BoneKind::Butt && Settings::game().mJiggleGlutePhysics;
        std::optional<osg::Matrix> skeletonWorld;
        if (glute && !nodePath.empty())
            skeletonWorld = findSkeletonWorld(nodePath);
        const osg::Vec3f gluteOffset = glute ? gluteRestOffset(node->getName(), skeletonWorld) : osg::Vec3f();

        const double simTime = nv->getFrameStamp() ? nv->getFrameStamp()->getSimulationTime() : 0.0;
        // Debug: say (every few seconds) when the spring is skipped, and why.
        auto logSkip = [&](const char* reason) {
            if (!mDebug || simTime - mLastSkipLog < 3.0)
                return;
            mLastSkipLog = simTime;
            Log(Debug::Warning) << "Jiggle bone skipped: actor=" << (mIsPlayer ? std::string("player") : mActorName)
                                << " ref=" << mRefNum.toString() << " node=" << node->getName() << " reason=" << reason;
        };

        if (!mInitialized)
        {
            // Cache the true NIF bind-pose local matrix exactly once. We must not
            // re-read node->getMatrix() on later frames: this same function just
            // wrote the jiggled position into that matrix, so re-reading it would
            // mean "rest" drifts to wherever the spring last left off, collapsing
            // the restoring force to ~0 after the very first frame.
            mRestLocalMatrix = node->getMatrix();
            const osg::Vec3f simulationOffset(0.f, 0.f, 0.f);
            osg::Vec3f restWorldPos = (mRestLocalMatrix.getTrans() + simulationOffset) * parentWorldMatrix;
            restWorldPos.z() += zOffsetFor(node->getName());
            restWorldPos += gluteOffset;
            mOffset = osg::Vec3f(0, 0, 0);
            mVelocity = osg::Vec3f(0, 0, 0);
            mHasRestVelocity = false;
            mPreviousRestWorldPos = restWorldPos;
            mInitialized = true;
            mLastSimTime = simTime;
            traverse(node, nv);
            return;
        }

        const osg::Vec3f restTranslation = mRestLocalMatrix.getTrans();
        const osg::Vec3f simulationOffset(0.f, 0.f, 0.f);
        osg::Vec3f restWorldPos = (restTranslation + simulationOffset) * parentWorldMatrix;
        restWorldPos.z() += zOffsetFor(node->getName());
        restWorldPos += gluteOffset;

        // Actor-specific Jiggle rules are live.
        if (!Misc::JigglePolicy::actorEnabled(mIsPlayer, mActorName, mRefNum))
        {
            logSkip("actor not enabled");
            mOffset = osg::Vec3f(0, 0, 0);
            mVelocity = osg::Vec3f(0, 0, 0);
            mHasRestVelocity = false;
            mPreviousRestWorldPos = restWorldPos;
            mLastSimTime = simTime;
            const osg::Vec3f newLocalTranslation
                = restWorldPos * osg::Matrix::inverse(parentWorldMatrix) - simulationOffset;
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

        const osg::Vec3f restStep = restWorldPos - mPreviousRestWorldPos;
        mPreviousRestWorldPos = restWorldPos;

        if (restStep.length2() > sTeleportResetDistance * sTeleportResetDistance)
        {
            logSkip("teleport");
            // Fast travel, cell transitions and scripted teleports can move an actor hundreds or
            // thousands of units in one update. Treat that as a discontinuity: snap the simulated
            // point to the new animated rest target and clear momentum so the cape cannot slingshot.
            mOffset = osg::Vec3f(0, 0, 0);
            mVelocity = osg::Vec3f(0, 0, 0);
            mHasRestVelocity = false;
            mLastSimTime = simTime;
            const osg::Vec3f newLocalTranslation
                = restWorldPos * osg::Matrix::inverse(parentWorldMatrix) - simulationOffset;
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

        // Live master off-switch: the "jiggle auto rig" toggle freezes spring motion, but must
        // leave the manual NIF bone mover active. This lets the Jiggle-tab breast slider position
        // an existing .nif breast bone live even when procedural auto-rigging is disabled.
        if (!Settings::game().mJiggleAutoRig)
        {
            logSkip("auto rig off");
            mOffset = osg::Vec3f(0, 0, 0);
            mVelocity = osg::Vec3f(0, 0, 0);
            mHasRestVelocity = false;
            mLastSimTime = simTime;
            const osg::Vec3f newLocalTranslation
                = restWorldPos * osg::Matrix::inverse(parentWorldMatrix) - simulationOffset;
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
            logSkip("no time step");
            traverse(node, nv);
            return;
        }
        // How fast the rest point (the body) moved this frame, over the real frame time, and how sharply that
        // changed: the body's acceleration is what throws the bone about (its inertia, felt in the body's frame).
        const osg::Vec3f restVelocity = restStep / static_cast<float>(dt);
        osg::Vec3f restAccel;
        if (mHasRestVelocity)
        {
            restAccel = (restVelocity - mRestVelocity) / static_cast<float>(dt);
            // a single hitch (a stair step, a frame spike) must not fling the bone across the screen
            if (restAccel.length2() > sMaxRestAccel * sMaxRestAccel)
            {
                restAccel.normalize();
                restAccel *= sMaxRestAccel;
            }
        }
        mRestVelocity = restVelocity;
        mHasRestVelocity = true;
        if (dt > sMaxDeltaTime)
            dt = sMaxDeltaTime;
        const float fdt = static_cast<float>(dt);

        const float baseStiffness = Settings::game().mJiggleBoneStiffness;
        const float baseDamping = Settings::game().mJiggleBoneDamping;
        float maxDisplacement = Settings::game().mJiggleBoneMaxDisplacement;
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
        float stiffness = baseStiffness * std::max(0.05f, stiffnessMult);
        float damping = baseDamping * std::max(0.f, dampingMult);

        // Gravity as a real world-space acceleration (down = world -Z). Because the spring solves
        // in world space, the resulting droop stays world-down and, when converted back into the
        // (rotating) parent's local frame, becomes an orientation-dependent forward/back/side shift
        // - reproducing TittyMagic's whole up/down/forward/back/left-right gravity family from one
        // physical term. Heavier+softer bones sag more since equilibrium droop = gravity/stiffness.
        float gravity = Settings::game().mJiggleBoneGravity;
        float mass = 1.f;

        // Glute depth axis: straight out of the buttocks (skeleton -X, i.e. backwards), falling back
        // to the radial from the parent joint for pre-rigged bones that carry a rest offset.
        std::optional<osg::Vec3f> depthAxis;
        if (glute && skeletonWorld)
        {
            osg::Vec3f back = osg::Matrix::transform3x3(osg::Vec3f(-1.f, 0.f, 0.f), *skeletonWorld);
            if (back.normalize() > 1e-6f)
                depthAxis = back;
        }
        if (!depthAxis)
        {
            osg::Vec3f radial = restWorldPos - parentWorldMatrix.getTrans();
            if (radial.normalize() > 1e-3f)
                depthAxis = radial;
        }

        // Naturalis BootyMagic joint model: separate swing (rotation spring/damper) and in/out
        // (depth spring/damper) response, glute mass, and depth in/out force multipliers. Replaces
        // the shared breast softness/quickness/mass-response shaping for butt bones.
        const bool gluteResponse = glute;
        JiggleGlute::Response gluteParams;
        float depthStiffness = stiffness;
        float depthDamping = damping;
        if (gluteResponse)
        {
            const float gluteMass = Settings::game().mJiggleGluteAutoMass
                ? std::clamp(sizeHint / 12.f, 0.f, 1.f)
                : JiggleGlute::inverseLerp(
                    JiggleGlute::sMassMin, JiggleGlute::sMassMax, Settings::game().mJiggleGluteMass);
            gluteParams = JiggleGlute::computeResponse(
                gluteMass, Settings::game().mJiggleGluteSoftness, Settings::game().mJiggleGluteQuickness);
            stiffness = baseStiffness * gluteParams.mTangentialStiffness;
            damping = baseDamping * gluteParams.mTangentialDamping;
            depthStiffness = baseStiffness * gluteParams.mDepthStiffness;
            depthDamping = baseDamping * gluteParams.mDepthDamping;
            mass *= gluteParams.mMass;
            gravity *= Settings::game().mJiggleGluteGravity;
            maxDisplacement *= gluteParams.mMaxDisplacement;
        }
        // Gravity never pulls the bone further down than max displacement at rest (the steady sag, gravity /
        // stiffness, is capped there), so a soft spring hangs at the limit and swings freely around it instead of
        // straining against it.
        if (gravity > 0.f && gravity / std::max(stiffness, 1e-3f) > maxDisplacement)
            gravity = maxDisplacement * stiffness;
        const osg::Vec3f gravityAccel(0.f, 0.f, -gravity);

        // The spring runs in the body's frame: mOffset is the bone's offset from its rest point and mVelocity its
        // velocity relative to the body. (It used to run on world positions, stepping the bone one frame ahead
        // and comparing it with this frame's rest point, which left it a frame's worth of movement ahead of the
        // body - pressed against the limit in any fast motion - and damped world velocity, which dragged it back
        // by about as much at walking pace.)
        osg::Vec3f displacement = mOffset;
        osg::Vec3f springForce = displacement * -stiffness;
        osg::Vec3f damperForce = mVelocity * -damping;
        if (gluteResponse && depthAxis)
        {
            // Split into the depth (in/out) component and the swing (tangential) remainder.
            const osg::Vec3f depthDisp = *depthAxis * (displacement * *depthAxis);
            const osg::Vec3f depthVel = *depthAxis * (mVelocity * *depthAxis);
            springForce = (displacement - depthDisp) * -stiffness + depthDisp * -depthStiffness;
            damperForce = (mVelocity - depthVel) * -damping + depthVel * -depthDamping;
        }
        const osg::Vec3f acceleration = (springForce + damperForce + gravityAccel) / mass - restAccel;
        mVelocity += acceleration * fdt;
        mOffset += mVelocity * fdt;

        // Self/body-collision containment: don't let the bone sink toward its parent joint (into
        // the torso) past the limit, and stop its inward velocity at that wall. Bone-space analog
        // of TittyMagic's soft self-collision + distance limit. Uses the outward radial from the
        // parent joint (or, for glutes, the depth axis), so it needs no knowledge of the skeleton's
        // axis convention.
        if (Settings::game().mJiggleBoneSelfCollision)
        {
            const float selfCollisionLimit = Settings::game().mJiggleBoneSelfCollisionLimit;
            const osg::Vec3f parentOriginWorld = parentWorldMatrix.getTrans();
            osg::Vec3f outwardWorld = restWorldPos - parentOriginWorld;
            float outwardLen = outwardWorld.length();
            if (glute && depthAxis)
            {
                outwardWorld = *depthAxis;
                outwardLen = 1.f;
            }
            if (outwardLen > 1e-3f)
            {
                outwardWorld /= outwardLen;
                const osg::Vec3f dispW = mOffset;
                const float inward = -(dispW * outwardWorld); // >0 when moving toward the body
                if (inward > selfCollisionLimit)
                {
                    mOffset += outwardWorld * (inward - selfCollisionLimit);
                    const float inwardVel = -(mVelocity * outwardWorld);
                    if (inwardVel > 0.f)
                        mVelocity += outwardWorld * inwardVel; // cancel the into-body velocity
                }
            }
        }

        displacement = mOffset;

        // Max displacement limits the swing around where gravity lets the bone hang, not the hang plus the
        // swing: the steady sag (the spring's equilibrium, gravity / stiffness, capped above) gets its own
        // allowance. Measuring the limit from the rest pose let the sag use up most of it, so in motion (walking,
        // running, animations) the bone sat pressed against the limit and the stiffness/softness/damping sliders
        // made no visible difference.
        const osg::Vec3f sag = gravityAccel / std::max(stiffness, 1e-3f);
        osg::Vec3f swing = displacement - sag;
        // The simulation itself may overshoot the limit (up to twice it, a safety bound) so the bounce keeps its
        // energy; only the rendered swing is limited, softly (see below).
        const float hardLimit = 2.f * maxDisplacement;
        if (swing.length2() > hardLimit * hardLimit)
        {
            swing.normalize();
            swing *= hardLimit;
            mOffset = sag + swing;
        }
        // Soft limit: the rendered swing eases towards max displacement (max * tanh(swing / max)): close to the
        // simulated swing for small motion, and never stopping dead against a wall, so a bigger swing always shows
        // as a bigger one (fast animations no longer all look the same, pressed against the limit).
        const float swingLen = swing.length();
        const bool clamped = swingLen > 0.8f * maxDisplacement;
        if (maxDisplacement <= 0.f)
            swing = osg::Vec3f();
        else if (swingLen > 1e-6f)
            swing *= maxDisplacement * std::tanh(swingLen / maxDisplacement) / swingLen;
        displacement = sag + swing;

        // Naturalis glute force physics: how strongly motion pushes the glute into the body (depth
        // in) or pulls it out (depth out). Shapes the rendered displacement along the depth axis.
        if (gluteResponse && depthAxis)
        {
            const float along = displacement * *depthAxis;
            const float scale = along >= 0.f
                ? gluteParams.mDepthOut * Settings::game().mJiggleGluteDepthOut
                : gluteParams.mDepthIn * Settings::game().mJiggleGluteDepthIn;
            displacement += *depthAxis * (along * (scale - 1.f));
        }

        // "Side sway" scales the horizontal (world XY) part of the jiggle relative to the
        // vertical bounce, so side-to-side / forward-back motion can be emphasised or damped
        // independently of up-down. 1.0 = uniform (old behaviour); 0 = vertical bounce only.
        const float side = Settings::game().mJiggleBoneSide;
        const osg::Vec3f shapedDisplacement(displacement.x() * side, displacement.y() * side, displacement.z());
        const osg::Vec3f visualWorldPos = restWorldPos + shapedDisplacement * intensity;

        const osg::Matrix parentWorldInverse = osg::Matrix::inverse(parentWorldMatrix);
        const osg::Vec3f newLocalTranslation = visualWorldPos * parentWorldInverse - simulationOffset;

        // Per-second summary: how far the bone sits from rest on average, how far it swings (travel of the
        // rendered offset per second) and how often the simulated swing is past 80% of max displacement.
        if (mDebug)
        {
            DebugStats& st = mStats;
            if (st.mStart < 0.0)
                st.mStart = simTime;
            const float len = displacement.length();
            if (st.mFrames > 0)
                st.mTravel += (displacement - st.mLast).length();
            st.mLast = displacement;
            ++st.mFrames;
            st.mSum += len;
            st.mMax = std::max(st.mMax, len);
            st.mClamped += clamped ? 1 : 0;
            const float speed = restVelocity.length();
            st.mSpeed += speed;
            if (speed > 1.f)
                st.mLagAlong += (displacement - sag) * (restVelocity / speed);
            if (simTime - st.mStart >= 1.0)
            {
                Log(Debug::Warning) << "Jiggle bone stats: actor=" << (mIsPlayer ? std::string("player") : mActorName)
                                    << " ref=" << mRefNum.toString() << " pos=" << restWorldPos.x() << ","
                                    << restWorldPos.y() << "," << restWorldPos.z() << " node=" << node->getName()
                                    << " frames=" << st.mFrames << " meanDisp=" << st.mSum / st.mFrames
                                    << " maxDisp=" << st.mMax << " clampedPct=" << 100 * st.mClamped / st.mFrames
                                    << " travelPerSec=" << st.mTravel / (simTime - st.mStart)
                                    << " max=" << maxDisplacement << " lagAlong=" << st.mLagAlong / st.mFrames
                                    << " speed=" << st.mSpeed / st.mFrames;
                st = DebugStats();
                st.mLast = displacement;
            }
        }
        // Runtime proof that the final transform which drives skinned cape vertices actually moved,
        // after displacement clamp, side shaping and visual intensity have all been applied.
        if (mDebug && (mDebugCounter++ % 60) == 0)
        {
            Log(Debug::Warning) << "Jiggle bone debug: node=" << node->getName() << " dt=" << dt
                                << " restWorldPos=" << restWorldPos.x() << "," << restWorldPos.y() << ","
                                << restWorldPos.z() << " offset=" << mOffset.x() << "," << mOffset.y() << ","
                                << mOffset.z() << " displacementLen=" << displacement.length()
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
