// Bru's eleven-part body and fragment ownership, simulated by Jolt.
// Derived from sunhawken/bru (GPL-3.0); see CREDITS.md.
#include "ragdoll.hpp"
#include "../mwworld/livecellref.hpp"
#include "creaturerig.hpp"
#include "joltlayers.hpp"
#include "mtphysics.hpp"
#include "ragdollpose.hpp"
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <components/misc/brutalconstants.hpp>
#include <components/misc/convert.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/settings/values.hpp>
#include <limits>
#include <stdexcept>

namespace MWPhysics
{
    namespace
    {
        struct Definition
        {
            const char* bone;
            const char* end;
            float radius, mass;
            int parent;
            float swing1, swing2, twist, density;
        };
        constexpr Definition definitions[] = { { "Bip01 Pelvis", "Bip01 Spine1", 11, 12, -1, 0, 0, 0, .8f },
            { "Bip01 Spine1", "Bip01 Neck", 12, 15, 0, 25, 25, 15, .65f },
            { "Bip01 Head", "", 8, 5, 1, 40, 40, 40, .95f },
            { "Bip01 L UpperArm", "Bip01 L Forearm", 4.5f, 3, 1, 80, 80, 40, .98f },
            { "Bip01 L Forearm", "Bip01 L Hand", 4, 2, 3, 70, 10, 20, .98f },
            { "Bip01 R UpperArm", "Bip01 R Forearm", 4.5f, 3, 1, 80, 80, 40, .98f },
            { "Bip01 R Forearm", "Bip01 R Hand", 4, 2, 5, 70, 10, 20, .98f },
            { "Bip01 L Thigh", "Bip01 L Calf", 6.5f, 7, 0, 60, 40, 15, .98f },
            { "Bip01 L Calf", "Bip01 L Foot", 5, 4, 7, 70, 10, 10, .98f },
            { "Bip01 R Thigh", "Bip01 R Calf", 6.5f, 7, 0, 60, 40, 15, .98f },
            { "Bip01 R Calf", "Bip01 R Foot", 5, 4, 9, 70, 10, 10, .98f } };
        constexpr unsigned roots[] = { 8, 10, 3, 5 };
        std::atomic<unsigned> nextGroup{ 0x100000 };
        bool finite(const osg::Vec3f& v)
        {
            return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
        }
        bool finite(const osg::Matrixf& m)
        {
            for (int i = 0; i < 16; ++i)
                if (!std::isfinite(m.ptr()[i]))
                    return false;
            return true;
        }
        osg::Matrixf matrix(const JPH::RVec3& p, const JPH::Quat& q)
        {
            return osg::Matrixf::rotate(Misc::Convert::toOsg(q)) * osg::Matrixf::translate(Misc::Convert::toOsg(p));
        }
        unsigned limbFragment(unsigned i, unsigned mask)
        {
            if ((mask & 1) && i == 8)
                return 1;
            if ((mask & 2) && i == 10)
                return 2;
            if ((mask & 4) && (i == 3 || i == 4))
                return 3;
            if ((mask & 8) && (i == 5 || i == 6))
                return 4;
            return 0;
        }
    }
    struct Ragdoll::Part : PtrHolder
    {
        explicit Part(const MWWorld::Ptr& ptr, Ragdoll& owner)
            : PtrHolder(ptr, {})
            , owner(owner)
        {
        }
        Ragdoll& owner;
        void onContactAdded(
            const JPH::Body& other, const JPH::ContactManifold& manifold, JPH::ContactSettings&) override
        {
            if (!owner.mSuperKickToken || other.GetObjectLayer() != Layers::ACTOR
                || other.GetID() == owner.mSuperKickAttacker || !mPhysicsBody)
                return;
            const auto relative = mPhysicsBody->GetLinearVelocity() - other.GetLinearVelocity();
            if (relative.Length() < 80.f)
                return;
            std::lock_guard lock(owner.mContactMutex);
            if (!owner.mSuperKickHitBodies.insert(other.GetID().GetIndexAndSequenceNumber()).second)
                return;
            auto transfer = relative * .22f;
            if (transfer.Length() > 300.f)
                transfer *= 300.f / transfer.Length();
            owner.mSuperKickContacts.push_back(
                { owner.mSuperKickToken, other.GetID(), Misc::Convert::toOsg(transfer) });
        }
        std::vector<JPH::BodyID> ignoredBodies;
        bool onContactValidate(const JPH::Body& other) override
        {
            return std::find(ignoredBodies.begin(), ignoredBodies.end(), other.GetID()) == ignoredBodies.end();
        }
        void retireSeparatedBodies(const JPH::BodyLockInterface& locks)
        {
            const auto bounds = mPhysicsBody->GetWorldSpaceBounds();
            std::erase_if(ignoredBodies, [&](JPH::BodyID id) {
                JPH::BodyLockRead lock(locks, id);
                return !lock.Succeeded() || !bounds.Overlaps(lock.GetBody().GetWorldSpaceBounds());
            });
        }
        std::string bone;
        osg::Matrixf renderFromBody;
        float mass = 1, radius = 1, halfLength = 1, density = 1;
        int parent = -1;
        JPH::Ref<JPH::Shape> shape;
        void assign(JPH::Body* body) { mPhysicsBody = body; }
    };
    const std::vector<std::string>& Ragdoll::getRequiredBones()
    {
        static const auto names = [] {
            std::vector<std::string> out;
            for (auto& d : definitions)
            {
                out.emplace_back(d.bone);
                if (*d.end)
                    out.emplace_back(d.end);
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }();
        return names;
    }
    Ragdoll::BoneMotion Ragdoll::sampleAnimationMotion(
        const BoneMatrices& previous, const BoneMatrices& current, const osg::Vec3f& translation, float dt)
    {
        BoneMotion out;
        if (!(dt > 0 && dt <= .25f) || !finite(translation))
            return out;
        for (const auto& [name, now] : current)
            if (auto old = previous.find(name); old != previous.end())
            {
                osg::Vec3f linear = (now.getTrans() - old->second.getTrans() - translation) / dt;
                osg::Quat delta = old->second.getRotate().inverse() * now.getRotate();
                double angle;
                osg::Vec3d axis;
                delta.getRotate(angle, axis);
                if (angle > osg::PI)
                    angle -= 2 * osg::PI;
                osg::Vec3f angular = axis * (angle / dt);
                if (finite(linear) && finite(angular))
                {
                    if (linear.length() > 500)
                        linear *= 500 / linear.length();
                    if (angular.length() > 20)
                        angular *= 20 / angular.length();
                    out.emplace(name, std::pair{ linear, angular });
                }
            }
        return out;
    }
    Ragdoll::Ragdoll(const MWWorld::Ptr& ptr, const BoneMatrices& bones, const osg::Vec3f& velocity,
        const osg::Vec3f& kick, PhysicsTaskScheduler* scheduler, const std::optional<osg::Vec3f>& contact,
        const BoneMotion& motion, const ExtremityVertices& surfaces, const UnsupportedExtremityBones& unsupported,
        KickProfile profile)
        : mTaskScheduler(scheduler)
        , mSystem(&scheduler->getPhysicsSystem())
        , mInitialRootVelocity(velocity)
        , mInheritedVelocity(velocity)
        , mUnsupportedExtremityBones(unsupported)
    {
        CreatureRig rig;
        for (const auto& name : getRequiredBones())
            if (!bones.contains(name) || !finite(bones.at(name)))
                throw std::runtime_error("Jolt ragdoll requires finite NPC bone: " + name);
        for (const auto& d : definitions)
        {
            auto start = bones.at(d.bone).getTrans();
            osg::Vec3f end;
            if (*d.end)
                end = bones.at(d.end).getTrans();
            else
            {
                auto up = start - bones.at("Bip01 Neck").getTrans();
                if (up.normalize() < .001f)
                    up = { 0, 0, 1 };
                end = start + up * 14;
            }
            auto inverse = osg::Matrixf::inverse(bones.at(d.bone));
            auto axis = osg::Matrixf::transform3x3(end - start, inverse);
            axis.normalize();
            rig.mParts.push_back({ d.bone, d.parent, ((start + end) * .5f) * inverse, axis,
                std::max(.05f, (end - start).length() * .5f - d.radius), d.radius, d.mass, d.swing1, d.swing2,
                d.twist });
        }
        build(ptr, rig, bones, velocity, motion, surfaces);
        if (kick.length2() > 0)
            applyKick(
                profile == KickProfile::LowTrip ? 8 : 1, kick, contact.value_or(bones.at("Bip01 Spine1").getTrans()));
        refresh();
        mInitialCOMVelocity = getDynamics()[1];
    }
    Ragdoll::Ragdoll(const MWWorld::Ptr& ptr, const CreatureRig& rig, const BoneMatrices& bones,
        const osg::Vec3f& velocity, const osg::Vec3f& kick, PhysicsTaskScheduler* scheduler,
        const std::optional<osg::Vec3f>& contact, KickProfile)
        : mTaskScheduler(scheduler)
        , mSystem(&scheduler->getPhysicsSystem())
        , mInitialRootVelocity(velocity)
        , mInheritedVelocity(velocity)
        , mCreature(true)
    {
        mKickScale = rig.kickScale(ptr.getCellRef().getScale());
        build(ptr, rig, bones, velocity, {});
        if (kick.length2() > 0)
            applyKick(0, kick, contact.value_or(bones.at(rig.mParts[0].mBone).getTrans()));
        refresh();
        mInitialCOMVelocity = getDynamics()[1];
    }
    void Ragdoll::build(const MWWorld::Ptr& ptr, const CreatureRig& rig, const BoneMatrices& bones,
        const osg::Vec3f& velocity, const BoneMotion& motion, const ExtremityVertices& surfaces)
    {
        if (rig.mParts.empty() || !finite(velocity))
            throw std::runtime_error("Invalid Jolt ragdoll input");
        mTaskScheduler->syncSimulation();
        mSettings = new JPH::RagdollSettings;
        mSettings->mSkeleton = new JPH::Skeleton;
        mActorOffset = ptr.getRefData().getPosition().asVec3() - bones.at(rig.mParts[0].mBone).getTrans();
        for (std::size_t i = 0; i < rig.mParts.size(); ++i)
        {
            const auto& r = rig.mParts[i];
            const auto& bone = bones.at(r.mBone);
            if (!finite(bone) || r.mParent >= int(i) || r.mRadius <= 0 || r.mMass <= 0)
                throw std::runtime_error("Invalid Jolt rig part");
            auto p = std::make_unique<Part>(ptr, *this);
            p->bone = r.mBone;
            p->parent = r.mParent;
            p->mass = r.mMass;
            p->density = mCreature ? 1.05f : definitions[i].density;
            p->radius = r.mRadius;
            p->halfLength = r.mHalfLength;
            auto axis = osg::Matrixf::transform3x3(r.mAxis, bone);
            axis.normalize();
            osg::Quat rotation;
            rotation.makeRotate(osg::Vec3f(0, 1, 0), axis);
            const osg::Vec3f center = r.mCenter * bone;
            p->shape = new JPH::CapsuleShape(std::max(.05f, r.mHalfLength), r.mRadius);
            if (!mCreature && Settings::game().mRagdollEnhancement && (i == 4 || i == 6 || i == 8 || i == 10))
            {
                const auto surface = surfaces.find(definitions[i].end);
                if (surface != surfaces.end() && !surface->second.empty())
                {
                    const auto inverse
                        = osg::Matrixf::inverse(osg::Matrixf::rotate(rotation) * osg::Matrixf::translate(center));
                    JPH::ConvexHullShapeSettings hull;
                    hull.mMaxConvexRadius = .15f;
                    const float radius = r.mRadius / (std::cos(osg::PI / 16) * std::cos(osg::PI / 16));
                    for (int end : { -1, 1 })
                        for (int latitude = 0; latitude <= 8; ++latitude)
                            for (int longitude = 0; longitude < 16; ++longitude)
                            {
                                const float angle = osg::PI * latitude / 8;
                                const float around = 2 * osg::PI * longitude / 16;
                                hull.mPoints.emplace_back(radius * std::sin(angle) * std::cos(around),
                                    end * r.mHalfLength + radius * std::cos(angle),
                                    radius * std::sin(angle) * std::sin(around));
                            }
                    bool valid = true;
                    for (const auto& vertex : surface->second)
                    {
                        if (!finite(vertex))
                        {
                            valid = false;
                            break;
                        }
                        hull.mPoints.push_back(Misc::Convert::toJolt<JPH::Vec3>(vertex * inverse));
                    }
                    if (valid)
                    {
                        const auto result = hull.Create();
                        if (!result.HasError())
                            p->shape = result.Get();
                    }
                }
            }
            p->renderFromBody
                = bone * osg::Matrixf::inverse(osg::Matrixf::rotate(rotation) * osg::Matrixf::translate(center));
            JPH::RagdollSettings::Part settings;
            settings.SetShape(p->shape);
            settings.mPosition = Misc::Convert::toJolt<JPH::RVec3>(center);
            settings.mRotation = Misc::Convert::toJolt(rotation);
            settings.mMotionType = JPH::EMotionType::Dynamic;
            settings.mObjectLayer = Layers::DYNAMIC_WORLD;
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = r.mMass;
            settings.mFriction = .75f;
            settings.mRestitution = 0;
            settings.mLinearDamping = .05f;
            settings.mAngularDamping = .15f;
            settings.mMaxLinearVelocity = 2000;
            settings.mMaxAngularVelocity = 20;
            settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
            settings.mLinearVelocity = Misc::Convert::toJolt<JPH::Vec3>(velocity);
            if (auto it = motion.find(r.mBone); it != motion.end())
            {
                settings.mLinearVelocity += Misc::Convert::toJolt<JPH::Vec3>(it->second.first);
                settings.mAngularVelocity = Misc::Convert::toJolt<JPH::Vec3>(it->second.second);
            }
            if (r.mParent >= 0)
            {
                auto joint = new JPH::SwingTwistConstraintSettings;
                joint->mSpace = JPH::EConstraintSpace::WorldSpace;
                joint->mPosition1 = joint->mPosition2 = Misc::Convert::toJolt<JPH::RVec3>(bone.getTrans());
                joint->mTwistAxis1 = joint->mTwistAxis2 = Misc::Convert::toJolt<JPH::Vec3>(axis);
                osg::Vec3f normal = axis ^ osg::Vec3f(0, 0, 1);
                if (normal.normalize() < .001f)
                    normal = { 1, 0, 0 };
                joint->mPlaneAxis1 = joint->mPlaneAxis2 = Misc::Convert::toJolt<JPH::Vec3>(normal);
                joint->mNormalHalfConeAngle = JPH::DegreesToRadians(r.mSwing1);
                joint->mPlaneHalfConeAngle = JPH::DegreesToRadians(r.mSwing2);
                joint->mTwistMinAngle = -JPH::DegreesToRadians(r.mTwist);
                joint->mTwistMaxAngle = JPH::DegreesToRadians(r.mTwist);
                joint->mMaxFrictionTorque = 100;
                joint->mNumVelocityStepsOverride = 32;
                joint->mNumPositionStepsOverride = 8;
                settings.mToParent = joint;
            }
            mSettings->mParts.push_back(settings);
            mSettings->mSkeleton->AddJoint(r.mBone.c_str(), r.mParent);
            mParts.push_back(std::move(p));
        }
        mGroupFilter = new JPH::GroupFilterTable(mParts.size());
        for (unsigned i = 0; i < mParts.size(); ++i)
        {
            mSettings->mParts[i].mCollisionGroup.SetSubGroupID(i);
            mSettings->mParts[i].mCollisionGroup.SetGroupFilter(mGroupFilter);
            for (unsigned j = 0; j < i; ++j)
                if (mParts[i]->parent == int(j) || !Settings::game().mRagdollSelfCollision)
                    mGroupFilter->DisableCollision(i, j);
        }
        mSettings->CalculateBodyIndexToConstraintIndex();
        mSettings->CalculateConstraintIndexToBodyIdxPair();
        mRagdoll.reset(mSettings->CreateRagdoll(nextGroup++, 0, mSystem));
        if (!mRagdoll)
            throw std::runtime_error("Jolt ragdoll body allocation failed");
        mRagdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
        for (unsigned i = 0; i < mRagdoll->GetBodyCount(); ++i)
        {
            auto id = mRagdoll->GetBodyID(i);
            mBodies.push_back(id);
            mSystem->GetBodyInterface().SetUserData(id, reinterpret_cast<std::uintptr_t>(mParts[i].get()));
            JPH::BodyLockWrite lock(mSystem->GetBodyLockInterface(), id);
            if (lock.Succeeded())
                mParts[i]->assign(&lock.GetBody());
        }
        refresh();
        configureEnhancement();
    }
    void Ragdoll::RagdollDeleter::operator()(JPH::Ragdoll* body) const
    {
        if (!body)
            return;
        body->RemoveFromPhysicsSystem();
        delete body;
    }
    Ragdoll::~Ragdoll()
    {
        mTaskScheduler->syncSimulation();
        mRagdoll.reset();
    }
    void Ragdoll::refresh() const
    {
        mLastPoses.clear();
        mTransforms.clear();
        mLinear.clear();
        mAngular.clear();
        mMaxSpeed = mMaxAngularSpeed = 0;
        auto& bi = mSystem->GetBodyInterface();
        for (unsigned i = 0; i < mBodies.size(); ++i)
        {
            JPH::RVec3 p;
            JPH::Quat q;
            bi.GetPositionAndRotation(mBodies[i], p, q);
            auto world = matrix(p, q);
            mTransforms.push_back(world);
            mLastPoses.emplace_back(mParts[i]->bone, mParts[i]->renderFromBody * world);
            JPH::Vec3 v, w;
            bi.GetLinearAndAngularVelocity(mBodies[i], v, w);
            mLinear.push_back(Misc::Convert::toOsg(v));
            mAngular.push_back(Misc::Convert::toOsg(w));
            mMaxSpeed = std::max(mMaxSpeed, v.Length());
            mMaxAngularSpeed = std::max(mMaxAngularSpeed, w.Length());
        }
    }
    std::vector<std::pair<std::string, osg::Matrixf>> Ragdoll::getBonePoses(std::optional<float> interpolation) const
    {
        refresh();
        if (!interpolation || mPreviousTransforms.size() != mTransforms.size())
            return mLastPoses;
        const float alpha = std::clamp(*interpolation, 0.f, 1.f);
        auto result = mLastPoses;
        for (unsigned i = 0; i < result.size(); ++i)
        {
            osg::Quat rotation;
            rotation.slerp(alpha, mPreviousTransforms[i].getRotate(), mTransforms[i].getRotate());
            const auto position = mPreviousTransforms[i].getTrans() * (1.f - alpha) + mTransforms[i].getTrans() * alpha;
            result[i].second
                = mParts[i]->renderFromBody * osg::Matrixf::rotate(rotation) * osg::Matrixf::translate(position);
        }
        return result;
    }
    std::optional<osg::Vec3f> Ragdoll::getActorPosition(const osg::Vec3f&)
    {
        refresh();
        if (mLastPoses.empty())
            return {};
        return mLastPoses[0].second.getTrans() + mActorOffset;
    }
    MWWorld::Ptr Ragdoll::getPtr() const
    {
        return mParts.front()->getPtr();
    }
    void Ragdoll::updatePtr(const MWWorld::Ptr& ptr)
    {
        for (auto& p : mParts)
            p->updatePtr(ptr);
    }
    unsigned Ragdoll::fragment(unsigned i) const
    {
        if (mHeadDetached && i == 2)
            return 1;
        if (mTorsoDetached && i >= 1 && i <= 6)
            return 1;
        return limbFragment(i, mSeveredLimbsMask);
    }
    bool Ragdoll::detach(unsigned child)
    {
        if (!mRagdoll || child >= mBodies.size())
            return false;
        const int index = mSettings->GetConstraintIndexForBodyIndex(child);
        if (index < 0)
            return false;
        mRagdoll->GetConstraint(index)->SetEnabled(false);
        mRagdoll->Activate();
        return true;
    }
    bool Ragdoll::detachHead(const osg::Vec3f& velocity, const osg::Vec3f& angularImpulse)
    {
        if (mCreature || mTorsoDetached || mSeveredLimbsMask || !finite(velocity) || !finite(angularImpulse))
            return false;
        if (mHeadDetached)
            return true;
        mTaskScheduler->syncSimulation();
        refresh();
        const auto before = mTransforms[2];
        const auto linearBefore = mLinear[2];
        const auto angularBefore = mAngular[2];
        if (!detach(2))
            return false;
        mHeadDetached = true;
        mSuperKickToken = 0;
        auto& bodies = mSystem->GetBodyInterface();
        if (velocity.length2() > 0)
        {
            for (const auto id : mBodies)
                bodies.SetLinearVelocity(id, Misc::Convert::toJolt<JPH::Vec3>(velocity));
            bodies.SetLinearVelocity(mBodies[2],
                Misc::Convert::toJolt<JPH::Vec3>(velocity + osg::Vec3f(0, 0, Misc::BrutalHeadSeparationSpeed)));
            bodies.AddAngularImpulse(mBodies[2], Misc::Convert::toJolt<JPH::Vec3>(angularImpulse));
        }
        refresh();
        mHeadDetachVelocity = mLinear[2];
        mHeadDetachAngularVelocity = mAngular[2];
        mHeadDetachErrors = osg::Vec3f((mTransforms[2].getTrans() - before.getTrans()).length(),
            velocity.length2() > 0 ? 0.f : (mLinear[2] - linearBefore).length(),
            velocity.length2() > 0 ? 0.f : (mAngular[2] - angularBefore).length());
        updateFragmentCollisions();
        return true;
    }
    bool Ragdoll::detachTorso(const osg::Vec3f& velocity, const osg::Vec3f& separation, const std::string& saved)
    {
        if (mCreature || mHeadDetached || mSeveredLimbsMask || !finite(velocity) || !finite(separation))
            return false;
        if (mTorsoDetached)
            return true;
        mTaskScheduler->syncSimulation();
        if (!detach(1))
            return false;
        mTorsoDetached = true;
        mSuperKickToken = 0;
        if (!saved.empty())
        {
            mPoseRestored = restorePose(saved, true, 0);
            mPoseRejected = !mPoseRestored;
        }
        else if (velocity.length2() > 0)
            for (unsigned i = 0; i < mBodies.size(); ++i)
                mSystem->GetBodyInterface().SetLinearVelocity(
                    mBodies[i], Misc::Convert::toJolt<JPH::Vec3>(velocity + (fragment(i) ? separation : osg::Vec3f())));
        refresh();
        return true;
    }
    bool Ragdoll::detachLimbs(unsigned mask, const osg::Vec3f& velocity, const std::string& saved)
    {
        if (mCreature || mHeadDetached || mTorsoDetached || !mask || (mask & ~15u) || !finite(velocity))
            return false;
        mTaskScheduler->syncSimulation();
        const unsigned added = mask & ~mSeveredLimbsMask;
        for (unsigned j = 0; j < 4; ++j)
            if (added & (1u << j))
                if (!detach(roots[j]))
                    return false;
        mSeveredLimbsMask |= mask;
        mSuperKickToken = 0;
        if (!saved.empty())
        {
            mPoseRestored = restorePose(saved, false, mSeveredLimbsMask);
            mPoseRejected = !mPoseRestored;
        }
        else if (velocity.length2() > 0)
            for (unsigned i = 0; i < mBodies.size(); ++i)
                if (limbFragment(i, added))
                    mSystem->GetBodyInterface().SetLinearVelocity(
                        mBodies[i], Misc::Convert::toJolt<JPH::Vec3>(velocity));
        refresh();
        return true;
    }
    std::string Ragdoll::savePose(bool torso) const
    {
        if (mCreature || (!mTorsoDetached && torso) || (!mSeveredLimbsMask && !torso))
            return {};
        mTaskScheduler->syncSimulation();
        refresh();
        const auto* root = getPtr().getRefData().getBaseNode();
        if (!root)
            return {};
        const osg::Matrixf actor
            = osg::Matrixf::rotate(root->getAttitude()) * osg::Matrixf::translate(root->getPosition());
        return RagdollPose::encode(
            std::span(mTransforms.data(), torso ? 2 : mTransforms.size()), actor, torso ? 0 : mSeveredLimbsMask);
    }
    bool Ragdoll::restorePose(std::string_view bytes, bool torso, unsigned mask)
    {
        const auto* root = getPtr().getRefData().getBaseNode();
        if (!root)
            return false;
        const osg::Matrixf actor
            = osg::Matrixf::rotate(root->getAttitude()) * osg::Matrixf::translate(root->getPosition());
        const auto decoded = RagdollPose::decode(bytes, actor, torso ? 0 : mask);
        if (!decoded)
            return false;
        const auto& targets = *decoded;
        refresh();
        const auto old = mTransforms;
        for (unsigned i = 0; i < mBodies.size(); ++i)
        {
            const auto target = torso
                ? old[i] * osg::Matrixf::inverse(old[fragment(i) ? 1 : 0]) * targets[fragment(i) ? 1 : 0]
                : targets[i];
            mSystem->GetBodyInterface().SetPositionAndRotation(mBodies[i],
                Misc::Convert::toJolt<JPH::RVec3>(target.getTrans()), Misc::Convert::toJolt(target.getRotate()),
                JPH::EActivation::Activate);
        }
        return true;
    }
    std::string Ragdoll::getBisectionPoseData() const
    {
        return savePose(true);
    }
    std::string Ragdoll::getLimbSeveringPoseData() const
    {
        return savePose(false);
    }
    bool Ragdoll::isDetachedHeadPart(std::size_t i) const
    {
        return mHeadDetached && i == 2;
    }
    bool Ragdoll::applyKick(std::size_t part, const osg::Vec3f& velocity, const osg::Vec3f& point)
    {
        if (part >= mBodies.size() || !finite(velocity) || !finite(point))
            return false;
        mTaskScheduler->syncSimulation();
        refresh();
        const auto group = fragment(part);
        float mass = 0;
        osg::Vec3f center;
        for (unsigned i = 0; i < mBodies.size(); ++i)
            if (fragment(i) == group)
            {
                mass += mParts[i]->mass;
                center += mTransforms[i].getTrans() * mParts[i]->mass;
            }
        if (mass <= 0)
            return false;
        center /= mass;
        auto dv = velocity * mKickScale;
        auto spin = (point - center) ^ dv;
        spin *= 1.f / std::max(100.f, (point - center).length2());
        if (spin.length() > 4)
            spin *= 4 / spin.length();
        auto& bi = mSystem->GetBodyInterface();
        for (unsigned i = 0; i < mBodies.size(); ++i)
            if (fragment(i) == group)
            {
                bi.SetLinearVelocity(mBodies[i],
                    Misc::Convert::toJolt<JPH::Vec3>(mLinear[i] + dv + (spin ^ (mTransforms[i].getTrans() - center))));
                bi.SetAngularVelocity(mBodies[i], Misc::Convert::toJolt<JPH::Vec3>(mAngular[i] + spin));
                bi.ActivateBody(mBodies[i]);
            }
        mImpulse = dv * mass;
        mContact = point;
        ++mKickCount;
        refresh();
        return true;
    }
    bool Ragdoll::applyImpact(
        std::size_t part, const osg::Vec3f& point, const osg::Vec3f& impulse, std::uint64_t id, bool applied)
    {
        if (!finite(point) || !finite(impulse) || (id && mImpacts.contains(id)))
            return false;
        mTaskScheduler->syncSimulation();
        refresh();
        if (part == std::numeric_limits<std::size_t>::max())
        {
            float nearest = std::numeric_limits<float>::max();
            for (unsigned i = 0; i < mBodies.size(); ++i)
            {
                float d = (mTransforms[i].getTrans() - point).length2();
                if (d < nearest)
                {
                    nearest = d;
                    part = i;
                }
            }
        }
        if (part >= mBodies.size())
            return false;
        if (id)
            mImpacts.insert(id);
        if (!applied)
        {
            mSystem->GetBodyInterface().AddImpulse(
                mBodies[part], Misc::Convert::toJolt<JPH::Vec3>(impulse), Misc::Convert::toJolt<JPH::RVec3>(point));
            mImpulse = impulse;
            mContact = point;
        }
        const auto& game = Settings::game();
        const int parent = mParts[part]->parent;
        if (!mCreature && game.mRagdollEnhancement && game.mRagdollLimbResponse && parent >= 0
            && fragment(part) == fragment(parent))
        {
            auto torque
                = ((point - mTransforms[part].getTrans()) ^ impulse) * (.05f * game.mRagdollImpactStrength.get());
            const float length = torque.length();
            if (length > 2000.f)
                torque *= 2000.f / length;
            const auto angularImpulse = Misc::Convert::toJolt<JPH::Vec3>(torque);
            mSystem->GetBodyInterface().AddAngularImpulse(mBodies[part], angularImpulse);
            mSystem->GetBodyInterface().AddAngularImpulse(mBodies[parent], -angularImpulse);
        }
        return true;
    }
    bool Ragdoll::getEquipmentFrame(bool left, osg::Matrixf& frame, osg::Vec3f& linear, osg::Vec3f& angular) const
    {
        const unsigned i = left ? 4 : 6;
        if (mCreature || mBodies.size() <= i)
            return false;
        refresh();
        frame = mTransforms[i];
        linear = mLinear[i];
        angular = mAngular[i];
        return true;
    }
    std::vector<JPH::BodyID> Ragdoll::getEquipmentGripBodies() const
    {
        return mCreature ? std::vector<JPH::BodyID>{} : std::vector<JPH::BodyID>{ mBodies[4], mBodies[6] };
    }
    std::vector<osg::Vec3f> Ragdoll::getDynamics() const
    {
        refresh();
        osg::Vec3f c, v;
        float mass = 0;
        for (unsigned i = 0; i < mBodies.size(); ++i)
        {
            mass += mParts[i]->mass;
            c += mTransforms[i].getTrans() * mParts[i]->mass;
            v += mLinear[i] * mParts[i]->mass;
        }
        return { c / mass, v / mass, mInheritedVelocity, mInitialCOMVelocity, mImpulse, mContact,
            { mInitialPoseError, mMaxSpeed, mMaxAngularSpeed } };
    }
    float Ragdoll::bottom() const
    {
        refresh();
        float z = std::numeric_limits<float>::max();
        for (unsigned i = 0; i < mBodies.size(); ++i)
        {
            JPH::BodyLockRead lock(mSystem->GetBodyLockInterface(), mBodies[i]);
            if (lock.Succeeded())
                z = std::min(z, lock.GetBody().GetWorldSpaceBounds().mMin.GetZ());
        }
        return z;
    }
    float Ragdoll::retainedBottom() const
    {
        refresh();
        float z = std::numeric_limits<float>::max();
        for (unsigned i = 0; i < mBodies.size(); ++i)
            if (!fragment(i))
            {
                JPH::BodyLockRead lock(mSystem->GetBodyLockInterface(), mBodies[i]);
                if (lock.Succeeded())
                    z = std::min(z, lock.GetBody().GetWorldSpaceBounds().mMin.GetZ());
            }
        return z;
    }
    float Ragdoll::maxJointError() const
    {
        refresh();
        float error = 0;
        for (unsigned i = 1; i < mBodies.size(); ++i)
        {
            int parent = mParts[i]->parent;
            if (parent < 0 || fragment(i) != fragment(parent))
                continue;
            const auto* joint = static_cast<const JPH::SwingTwistConstraint*>(
                mRagdoll->GetConstraint(mSettings->GetConstraintIndexForBodyIndex(i)));
            const auto a = joint->GetConstraintToBody1Matrix().GetTranslation();
            const auto b = joint->GetConstraintToBody2Matrix().GetTranslation();
            const auto pa = Misc::Convert::toOsg(a) * mTransforms[parent];
            const auto pb = Misc::Convert::toOsg(b) * mTransforms[i];
            error = std::max(error, (pa - pb).length());
        }
        return error;
    }
    std::vector<osg::Vec3f> Ragdoll::getHeadState() const
    {
        refresh();
        if (mCreature || mBodies.size() < 3)
            return {};
        unsigned constraints = 0;
        for (unsigned i = 1; i < mBodies.size(); ++i)
            if (mParts[i]->parent >= 0 && fragment(i) == fragment(mParts[i]->parent))
                ++constraints;
        unsigned headPairs = 0;
        for (const auto& [a, b] : mSeparatedPairs)
            if (a == 2 || b == 2)
                ++headPairs;
        return { { float(mHeadDetached), float(constraints), float(headPairs) }, mHeadDetachErrors,
            mLastPoses[2].second.getTrans(), mLinear[2], mAngular[2], mHeadDetachVelocity, mHeadDetachAngularVelocity };
    }
    void Ragdoll::configureEnhancement()
    {
        mTaskScheduler->syncSimulation();
        const auto& game = Settings::game();
        const bool enabled = game.mRagdollEnhancement && !mCreature;
        const float tone = game.mRagdollToneStrength;
        if (!std::isfinite(tone))
            throw std::runtime_error("Non-finite ragdoll tone strength");
        for (unsigned i = 1; i < mBodies.size(); ++i)
        {
            const int index = mSettings->GetConstraintIndexForBodyIndex(i);
            if (index < 0)
                continue;
            auto* joint = static_cast<JPH::SwingTwistConstraint*>(mRagdoll->GetConstraint(index));
            joint->SetMaxFrictionTorque(enabled ? 100.f * tone : 100.f);
            const auto state = enabled && game.mRagdollNeutralPose && tone > 0.f ? JPH::EMotorState::Position
                                                                                 : JPH::EMotorState::Off;
            joint->GetSwingMotorSettings() = JPH::MotorSettings(1.f, 1.f, 0.f, 100.f * tone);
            joint->GetTwistMotorSettings() = JPH::MotorSettings(1.f, 1.f, 0.f, 100.f * tone);
            joint->SetSwingMotorState(state);
            joint->SetTwistMotorState(state);
        }
    }
    void Ragdoll::updateFragmentCollisions()
    {
        if (!mHeadDetached && !mTorsoDetached && !mSeveredLimbsMask)
            return;
        const auto& bodies = mSystem->GetBodyLockInterface();
        for (unsigned i = 0; i < mBodies.size(); ++i)
            for (unsigned j = 0; j < i; ++j)
            {
                if (fragment(i) == fragment(j))
                    continue;
                const auto pair = std::pair{ i, j };
                if (std::find(mSeparatedPairs.begin(), mSeparatedPairs.end(), pair) != mSeparatedPairs.end())
                    continue;
                // A cut initially shares a surface with the retained body. Keep
                // that pair excluded until it separates, then restore contacts.
                JPH::AABox first, second;
                {
                    JPH::BodyLockRead lock(bodies, mBodies[i]);
                    if (!lock.Succeeded())
                        continue;
                    first = lock.GetBody().GetWorldSpaceBounds();
                }
                {
                    JPH::BodyLockRead lock(bodies, mBodies[j]);
                    if (!lock.Succeeded())
                        continue;
                    second = lock.GetBody().GetWorldSpaceBounds();
                }
                if (first.Overlaps(second))
                    mGroupFilter->DisableCollision(i, j);
                else
                {
                    mGroupFilter->EnableCollision(i, j);
                    mSeparatedPairs.push_back(pair);
                }
            }
    }
    void Ragdoll::beforeStep(float dt)
    {
        refresh();
        mPreviousTransforms = mTransforms;
        for (auto& part : mParts)
            part->retireSeparatedBodies(mSystem->GetBodyLockInterface());
        updateFragmentCollisions();
        mAge += dt;
        const auto& game = Settings::game();
        if (!mCreature && game.mRagdollEnhancement)
            for (unsigned i = 1; i < mBodies.size(); ++i)
            {
                const int parent = mParts[i]->parent;
                if (parent < 0 || fragment(i) != fragment(parent))
                    continue;
                auto* joint = static_cast<JPH::SwingTwistConstraint*>(
                    mRagdoll->GetConstraint(mSettings->GetConstraintIndexForBodyIndex(i)));
                const auto rotation = joint->GetRotationInConstraintSpace();
                JPH::Quat swing, twist;
                rotation.GetSwingTwist(swing, twist);
                JPH::Vec3 axis;
                float angle;
                swing.GetAxisAngle(axis, angle);
                const float swingRatio
                    = std::hypot(angle * axis.GetY() / std::max(.01f, joint->GetPlaneHalfConeAngle()),
                        angle * axis.GetZ() / std::max(.01f, joint->GetNormalHalfConeAngle()));
                const float twistAngle = twist.GetRotationAngle(JPH::Vec3::sAxisX());
                const float twistLimit = twistAngle < 0 ? -joint->GetTwistMinAngle() : joint->GetTwistMaxAngle();
                const float ratio = std::max(swingRatio, std::abs(twistAngle) / std::max(.01f, twistLimit));
                const float soft = game.mRagdollSoftStops
                    ? std::clamp((ratio - .8f) / .2f, 0.f, 1.f) * game.mRagdollSoftStrength.get()
                    : 0.f;
                const float tone = game.mRagdollNeutralPose ? game.mRagdollToneStrength.get() : 0.f;
                const float strength = tone + soft;
                const auto state = strength > 0 ? JPH::EMotorState::Position : JPH::EMotorState::Off;
                joint->SetTargetOrientationCS(tone > 0 || ratio <= .8f
                        ? JPH::Quat::sIdentity()
                        : JPH::Quat::sIdentity().SLERP(rotation, .8f / ratio));
                joint->GetSwingMotorSettings() = JPH::MotorSettings(1.f + soft, 1.f, 0.f, 100.f * strength);
                joint->GetTwistMotorSettings() = JPH::MotorSettings(1.f + soft, 1.f, 0.f, 100.f * strength);
                joint->SetSwingMotorState(state);
                joint->SetTwistMotorState(state);
            }
        if (mSuperKickToken && (mSuperKickAge += dt) > 10.f)
            mSuperKickToken = 0;
        if (!mWaterEnabled)
            return;
        auto& bi = mSystem->GetBodyInterface();
        for (unsigned i = 0; i < mBodies.size(); ++i)
            bi.ApplyBuoyancyImpulse(mBodies[i], JPH::RVec3(0, 0, mWaterHeight), JPH::Vec3::sAxisZ(),
                1.f / mParts[i]->density, .5f, .2f, JPH::Vec3::sZero(), mSystem->GetGravity(), dt);
    }
    std::map<std::string, double> Ragdoll::getEnhancementState() const
    {
        return { { "simulatedAge", mAge }, { "limbsMask", mSeveredLimbsMask }, { "torsoDetached", mTorsoDetached },
            { "limbPoseRestored", mPoseRestored }, { "torsoPoseRestored", mPoseRestored },
            { "limbPosePayloadRejected", mPoseRejected }, { "torsoPosePayloadRejected", mPoseRejected } };
    }
    bool Ragdoll::startSuperKick(std::uint64_t token, std::uint64_t, const osg::Vec3f& velocity, JPH::BodyID attacker)
    {
        if (mHeadDetached || mTorsoDetached || mSeveredLimbsMask)
            return false;
        if (!applyKick(0, velocity, getDynamics()[0]))
            return false;
        mSuperKickToken = token;
        mSuperKickAttacker = attacker;
        mSuperKickAge = 0;
        return true;
    }
    std::map<std::string, double> Ragdoll::getSuperKickState() const
    {
        return { { "active", mSuperKickToken != 0 }, { "kickId", double(mSuperKickToken) } };
    }
    std::vector<Ragdoll::SuperKickContact> Ragdoll::takeSuperKickContacts()
    {
        mTaskScheduler->syncSimulation();
        std::vector<SuperKickContact> result;
        {
            std::lock_guard lock(mContactMutex);
            result.swap(mSuperKickContacts);
        }
        auto& bodies = mSystem->GetBodyInterface();
        for (const auto& contact : result)
            for (const auto id : mBodies)
                bodies.SetLinearVelocity(
                    id, bodies.GetLinearVelocity(id) - Misc::Convert::toJolt<JPH::Vec3>(contact.transfer));
        return result;
    }
    void Ragdoll::guardSuperKickSpawn()
    {
        mTaskScheduler->syncSimulation();
        JPH::BodyIDVector existing;
        mSystem->GetBodies(existing);
        for (unsigned i = 0; i < mBodies.size(); ++i)
        {
            JPH::AABox bounds;
            {
                JPH::BodyLockRead lock(mSystem->GetBodyLockInterface(), mBodies[i]);
                if (!lock.Succeeded())
                    continue;
                bounds = lock.GetBody().GetWorldSpaceBounds();
            }
            for (const auto id : existing)
            {
                if (std::find(mBodies.begin(), mBodies.end(), id) != mBodies.end())
                    continue;
                JPH::BodyLockRead lock(mSystem->GetBodyLockInterface(), id);
                if (!lock.Succeeded() || !lock.GetBody().GetUserData())
                    continue;
                auto* other = dynamic_cast<Part*>(reinterpret_cast<PtrHolder*>(lock.GetBody().GetUserData()));
                if (other && bounds.Overlaps(lock.GetBody().GetWorldSpaceBounds()))
                    mParts[i]->ignoredBodies.push_back(id);
            }
        }
        mRagdoll->Activate();
    }
    float Ragdoll::findKickPart(const osg::Vec3f& origin, const osg::Vec3f& forward, float reach, float minimumDot,
        std::size_t& part, osg::Vec3f& point) const
    {
        refresh();
        float best = -1;
        for (unsigned i = 0; i < mBodies.size(); ++i)
        {
            auto delta = mTransforms[i].getTrans() - origin;
            osg::Vec3f flat(delta.x(), delta.y(), 0);
            if (flat.normalize() < .001f || flat * forward < minimumDot)
                continue;
            float distance = delta.length();
            if (distance < .001f)
                continue;
            auto hit = origin + delta * (std::max(0.f, distance - mParts[i]->radius) / distance);
            distance = (hit - origin).length();
            if (distance <= reach && (best < 0 || distance < best))
            {
                best = distance;
                part = i;
                point = hit;
            }
        }
        return best;
    }
}
