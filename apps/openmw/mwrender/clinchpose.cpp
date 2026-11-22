#include "npcanimation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <components/sceneutil/skeleton.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwworld/class.hpp"

namespace MWRender
{
    namespace
    {
        constexpr float maximumCorrection = 28.f;
        constexpr float blendTime = .2f;

        std::optional<osg::Matrixf> nodeWorld(osg::Node* node)
        {
            if (!node)
                return std::nullopt;
            const auto paths = node->getParentalNodePaths();
            if (paths.empty())
                return std::nullopt;
            return osg::computeLocalToWorld(paths.front());
        }

        class BladeTipVisitor : public osg::NodeVisitor
        {
            osg::Matrixf mRootInverse;
        public:
            float tip = 0.f;
            explicit BladeTipVisitor(osg::Node* root)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mRootInverse(osg::Matrixf::inverse(osg::computeLocalToWorld(osg::NodePath{ root })))
            {
                setNodeMaskOverride(~0u);
            }
            void apply(osg::Geometry& geometry) override
            {
                const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
                if (!vertices)
                    return;
                // All model transforms are included, but the actor's animated/race
                // transform is excluded. Sword meshes extend along local +Y.
                const osg::Matrixf local = osg::computeLocalToWorld(getNodePath()) * mRootInverse;
                for (const auto& vertex : *vertices)
                {
                    const float y = (vertex * local).y();
                    if (std::isfinite(y))
                        tip = std::max(tip, y);
                }
            }
        };

        struct Contact
        {
            float a = 0.f, b = 0.f;
            osg::Vec3f delta;
            float distance2 = std::numeric_limits<float>::max();
        };

        Contact closestInterior(const osg::Vec3f& p, const osg::Vec3f& u, const osg::Vec3f& rangeA,
            const osg::Vec3f& q, const osg::Vec3f& w, const osg::Vec3f& rangeB)
        {
            Contact best;
            const float uu = u * u, ww = w * w, uw = u * w;
            if (uu < 1e-6f || ww < 1e-6f)
                return best;
            const osg::Vec3f d = q - p;
            const auto candidate = [&](float a, float b) {
                const osg::Vec3f delta = d + w * b - u * a;
                const float distance2 = delta.length2();
                if (distance2 < best.distance2)
                    best = { a, b, delta, distance2 };
            };
            // The constrained quadratic reaches its minimum either inside the
            // rectangle or on one of its four edges. This also handles parallel blades.
            const float determinant = uu * ww - uw * uw;
            if (determinant > 1e-6f * uu * ww)
            {
                const float a = ((d * u) * ww - (d * w) * uw) / determinant;
                const float b = ((d * u) * uw - (d * w) * uu) / determinant;
                if (a >= rangeA.x() && a <= rangeA.y() && b >= rangeB.x() && b <= rangeB.y())
                    candidate(a, b);
            }
            for (float a : { rangeA.x(), rangeA.y() })
                candidate(a, std::clamp(((p + u * a - q) * w) / ww, rangeB.x(), rangeB.y()));
            for (float b : { rangeB.x(), rangeB.y() })
                candidate(std::clamp(((q + w * b - p) * u) / uu, rangeA.x(), rangeA.y()), b);
            return best;
        }

        float smooth(float x)
        {
            x = std::clamp(x, 0.f, 1.f);
            return x * x * (3.f - 2.f * x);
        }
    }

    osg::Vec3f NpcAnimation::getClinchBladeRange()
    {
        osg::Node* node = getWeaponNode();
        if (node != mClinchBladeNode.get())
        {
            mClinchBladeNode = node;
            mClinchBladeRange = {};
            if (node)
            {
                BladeTipVisitor visitor(node);
                node->accept(visitor);
                // Exclude pommel, grip, guard and the unstable tip. The interval
                // scales with the actual equipped mesh, including short swords.
                if (visitor.tip >= 8.f && visitor.tip <= 250.f)
                    mClinchBladeRange = osg::Vec3f(std::max(3.f, visitor.tip * .25f), visitor.tip * .75f, visitor.tip);
            }
        }
        return mClinchBladeRange;
    }

    bool NpcAnimation::canFitSwordClinchPose(NpcAnimation& opponent)
    {
        return mViewMode == VM_FirstPerson && !useFullBodyFirstPerson() && !mBodyAwareness
            && getNode("Bip01 L Clavicle") && getNode("Bip01 R Clavicle")
            && getClinchBladeRange().y() > getClinchBladeRange().x()
            && opponent.getClinchBladeRange().y() > opponent.getClinchBladeRange().x()
            && nodeWorld(getWeaponNode()).has_value() && nodeWorld(opponent.getWeaponNode()).has_value();
    }

    void NpcAnimation::restoreTransientPose()
    {
        if (mClinchSourcePose.empty() && mSwordWallSourcePose.empty())
            return;
        for (const auto& [node, source] : mSwordWallSourcePose)
            if (node.valid())
                node->setMatrix(source);
        mSwordWallSourcePose.clear();
        for (const auto& [node, source] : mClinchSourcePose)
            if (node.valid())
                node->setMatrix(source);
        mClinchSourcePose.clear();
        if (mSkeleton)
            mSkeleton->markDirty();
    }

    void NpcAnimation::fitSwordClinchPose(osg::NodeVisitor& visitor)
    {
        // Refresh even on cancellation/death/paused frames without runAnimation.
        updateBodyAwarenessClinchVisibility();
        auto& audit = mSwordClinchGeometry;
        audit.active = audit.fitted = audit.clamped = false;
        audit.weight = 0.f;
        if (mPtr != MWMechanics::getPlayer() || mViewMode != VM_FirstPerson || useFullBodyFirstPerson()
            || mBodyAwareness || isRagdollPose() || !visitor.getFrameStamp())
        {
            mClinchWasContest = false;
            mClinchLastCorrection = {};
            mClinchExitCorrection = {};
            return;
        }
        auto& stats = mPtr.getClass().getCreatureStats(mPtr);
        if (stats.isDead())
            return;
        const double now = visitor.getFrameStamp()->getSimulationTime();
        const bool contest = stats.mBrutalClinchActive && stats.mBrutalClinchPhase <= 2;
        auto world = MWBase::Environment::get().getWorld();
        auto* opponent = stats.mBrutalClinchOpponent.isEmpty() ? nullptr
            : dynamic_cast<NpcAnimation*>(world->getAnimation(stats.mBrutalClinchOpponent));
        const auto playerMatrix = nodeWorld(getWeaponNode());
        const auto opponentMatrix = opponent ? nodeWorld(opponent->getWeaponNode()) : std::nullopt;
        osg::Vec3f correction;
        audit.frame = visitor.getTraversalNumber();
        audit.phase = stats.mBrutalClinchPhase;
        audit.elapsed = stats.mBrutalClinchElapsed;
        audit.sourceElapsed = contest
            ? std::max(0.f, getCurrentTime("brutal_clinch") - getTextKeyTime("brutal_clinch: start")) : 0.f;
        if (contest && opponent && playerMatrix && opponentMatrix && canFitSwordClinchPose(*opponent))
        {
            if (mClinchPoseId != stats.mBrutalClinchId)
            {
                mClinchPoseId = stats.mBrutalClinchId;
                mClinchEntryTime = now;
                mClinchExitCorrection = {};
            }
            // The pair is placed while the guard pose still holds. Fitting that pose to the blade
            // just placed would throw the arms at the camera, so the fit starts with the contest.
            if (stats.mBrutalClinchPhase < 2)
                mClinchEntryTime = now;
            audit.active = true;
            audit.playerHilt = playerMatrix->getTrans();
            audit.opponentHilt = opponentMatrix->getTrans();
            // OSG uses row vectors. Keep scale in the +Y axes: parameters remain
            // model-local units even with the NPC's anisotropic race weight.
            audit.playerAxis = osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), *playerMatrix);
            audit.opponentAxis = osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), *opponentMatrix);
            audit.playerRange = getClinchBladeRange();
            audit.opponentRange = opponent->getClinchBladeRange();
            const auto contact = closestInterior(audit.playerHilt, audit.playerAxis, audit.playerRange,
                audit.opponentHilt, audit.opponentAxis, audit.opponentRange);
            audit.rawGap = std::sqrt(contact.distance2);
            audit.requestedCorrection = contact.delta;
            correction = contact.delta;
            audit.clamped = correction.length2() > maximumCorrection * maximumCorrection;
            if (audit.clamped)
                correction *= maximumCorrection / correction.length();
            audit.weight = smooth(static_cast<float>(now - mClinchEntryTime) / blendTime);
            correction *= audit.weight;
            mClinchLastCorrection = correction;
            mClinchWasContest = true;
            // Contact fitting is visual only. It never changes the click
            // outcome or cancels an already accepted contest.
        }
        else
        {
            if (mClinchWasContest)
            {
                mClinchWasContest = false;
                mClinchExitTime = now;
                mClinchExitCorrection = mClinchLastCorrection;
            }
            const float fade = 1.f - smooth(static_cast<float>(now - mClinchExitTime) / .2f);
            correction = mClinchExitCorrection * fade;
            audit.weight = fade;
        }
        audit.correction = correction;
        if (correction.length2() < 1e-8f && !audit.active)
            return;

        const auto cameraBefore = getBoneWorldMatrix("Camera");
        const auto clavicleNode = [&](std::string_view name) -> osg::MatrixTransform* {
            const auto found = getNodeMap().find(std::string(name));
            return found == getNodeMap().end() ? nullptr : found->second.get();
        };
        std::array<osg::MatrixTransform*, 2> clavicles{
            clavicleNode("Bip01 L Clavicle"), clavicleNode("Bip01 R Clavicle") };
        if (!clavicles[0] || !clavicles[1])
            return;
        for (auto* clavicle : clavicles)
        {
            auto paths = clavicle->getParentalNodePaths();
            if (paths.empty())
            {
                restoreTransientPose();
                return;
            }
            paths.front().pop_back();
            const osg::Matrixf parentInverse = osg::Matrixf::inverse(osg::computeLocalToWorld(paths.front()));
            const osg::Matrixf source = clavicle->getMatrix();
            mClinchSourcePose.emplace_back(clavicle, source);
            osg::Matrixf fitted = source;
            fitted.setTrans(source.getTrans() + osg::Matrixf::transform3x3(correction, parentInverse));
            clavicle->setMatrix(fitted);
        }
        if (mSkeleton)
            mSkeleton->markDirty();
        // Refresh skin bounds with the fitted transforms but do not invoke any
        // keyframe/controller callbacks again. Actual skinning follows in CULL.
        osg::NodeVisitor bounds(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);
        bounds.setVisitorType(osg::NodeVisitor::UPDATE_VISITOR);
        bounds.setTraversalNumber(visitor.getTraversalNumber());
        bounds.setFrameStamp(new osg::FrameStamp(*visitor.getFrameStamp()));
        bounds.setNodeMaskOverride(~0u);
        mObjectRoot->accept(bounds);

        const auto cameraAfter = getBoneWorldMatrix("Camera");
        audit.cameraError = cameraBefore && cameraAfter
            ? (cameraAfter->getTrans() - cameraBefore->getTrans()).length() : 0.f;
        if (audit.active)
        {
            // Measure the final rendered matrices, not the requested offset.
            // This catches wrong ancestry, axis conventions and stale transforms.
            const auto actual = nodeWorld(getWeaponNode());
            const auto other = nodeWorld(opponent->getWeaponNode());
            if (actual && other)
            {
                audit.playerHilt = actual->getTrans();
                audit.opponentHilt = other->getTrans();
                audit.playerAxis = osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), *actual);
                audit.opponentAxis = osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), *other);
                const auto final = closestInterior(audit.playerHilt, audit.playerAxis, audit.playerRange,
                    audit.opponentHilt, audit.opponentAxis, audit.opponentRange);
                audit.playerParameter = final.a;
                audit.opponentParameter = final.b;
                audit.playerContact = audit.playerHilt + audit.playerAxis * final.a;
                audit.opponentContact = audit.opponentHilt + audit.opponentAxis * final.b;
                audit.finalGap = std::sqrt(final.distance2);
                audit.fitted = audit.finalGap < .15f && audit.cameraError < .001f;
            }
        }
    }
}
