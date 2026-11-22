#include "physicssystem.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include <osg/Group>
#include <osg/Stats>
#include <osg/Timer>

// IMPORTANT: Jolt/Jolt.h must be included first before any other Jolt headers
// It defines critical macros that other headers depend on
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/mathutil.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/conversion.hpp>
#include <components/resource/physicsshapemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/movement.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/player.hpp"

#include "../mwrender/joltdebugdraw.hpp"

#include "../mwworld/class.hpp"

#include "actor.hpp"
#include "collisionshapeconfig.hpp"
#include "dynamicobject.hpp"
#include "joltlayers.hpp"
#include "ragdollwrapper.hpp"
#include "ragdoll.hpp"
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/esm3/releasedequipmentstate.hpp>
#include <components/esm3/loadcrea.hpp>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include "creaturerig.hpp"
#include <components/misc/rng.hpp>
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwmechanics/character.hpp"
#include "../mwmechanics/damagesourcetype.hpp"

#include "heightfield.hpp"
#include "movementsolver.hpp"
#include "mtphysics.hpp"
#include "object.hpp"
#include "projectile.hpp"
#include "water.hpp"

#include "joltcallbacks.hpp"
#include "joltfilters.hpp"
#include "joltlisteners.hpp"

using namespace JPH;

// Callback for traces
static void TraceImpl(const char* inFMT, ...)
{
    // Format the message
    va_list list;
    va_start(list, inFMT);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), inFMT, list);
    va_end(list);

    Log(Debug::Info) << "Jolt Trace: " << buffer;
}

#ifdef JPH_ENABLE_ASSERTS

// Callback for Jolt asserts
static bool AssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, uint inLine)
{
    Log(Debug::Error) << "Jolt Assert: " << inFile << ":" << inLine << ": (" << inExpression << ") "
                      << (inMessage != nullptr ? inMessage : "");

    // Prevent breakpoint, better to log than exit/crash in debug mode usually
    // Jolt has a tendancy to complain about alot of things even if they work fine
    return true;
}

#endif

namespace
{
    void handleJump(const MWWorld::Ptr& ptr)
    {
        if (!ptr.getClass().isActor())
            return;
        if (ptr.getClass().getMovementSettings(ptr).mPosition[2] == 0)
            return;
        const bool isPlayer = (ptr == MWMechanics::getPlayer());
        // Advance acrobatics and set flag for GetPCJumping
        if (isPlayer)
        {
            ptr.getClass().skillUsageSucceeded(ptr, ESM::Skill::Acrobatics, ESM::Skill::Acrobatics_Jump);
            MWBase::Environment::get().getWorld()->getPlayer().setJumping(true);
        }

        // Decrease fatigue
        if (!isPlayer || !MWBase::Environment::get().getWorld()->getGodModeState())
        {
            const MWWorld::Store<ESM::GameSetting>& gmst
                = MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>();
            const float fFatigueJumpBase = gmst.find("fFatigueJumpBase")->mValue.getFloat();
            const float fFatigueJumpMult = gmst.find("fFatigueJumpMult")->mValue.getFloat();
            const float normalizedEncumbrance = std::min(1.f, ptr.getClass().getNormalizedEncumbrance(ptr));
            const float fatigueDecrease = fFatigueJumpBase + normalizedEncumbrance * fFatigueJumpMult;
            MWMechanics::DynamicStat<float> fatigue = ptr.getClass().getCreatureStats(ptr).getFatigue();
            fatigue.setCurrent(fatigue.getCurrent() - fatigueDecrease);
            ptr.getClass().getCreatureStats(ptr).setFatigue(fatigue);
        }
        ptr.getClass().getMovementSettings(ptr).mPosition[2] = 0;
    }

}

namespace MWPhysics
{
    namespace
    {
        // Object layers are split across broadphase trees.  A ray query has no
        // source body from which Jolt can infer the trees to inspect, so derive
        // them from the requested object-layer mask instead.
        MultiBroadPhaseLayerFilter getRaycastBroadPhaseLayerFilter(int mask)
        {
            std::vector<JPH::BroadPhaseLayer> layers;
            if (mask & (Layers::WORLD | Layers::DOOR | Layers::HEIGHTMAP | Layers::WATER | Layers::CAMERA_ONLY
                            | Layers::VISUAL_ONLY))
                layers.push_back(BroadPhaseLayers::WORLD);
            if (mask & (Layers::ACTOR | Layers::PROJECTILE | Layers::DYNAMIC_WORLD))
                layers.push_back(BroadPhaseLayers::MOVING);
            if (mask & Layers::DEBRIS)
                layers.push_back(BroadPhaseLayers::DEBRIS);
            if (mask & Layers::SENSOR)
                layers.push_back(BroadPhaseLayers::SENSOR);
            return MultiBroadPhaseLayerFilter(std::move(layers));
        }
    }

    // If you take larger steps than 1 / 60th of a second you need to do multiple collision steps in order to keep the
    // simulation stable. Do 1 collision step per 1 / 60th of a second (round up).
    static const int cCollisionSteps = 1;

    // This is the max amount of rigid bodies that you can add to the physics system
    static const unsigned int cMaxBodies = 65536;

    // This determines how many mutexes to allocate to protect rigid bodies from concurrent access. Set it to 0 for the
    // default settings.
    static const unsigned int cNumBodyMutexes = 0;

    // This is the max amount of body pairs that can be queued at any time (the broad phase will detect overlapping
    // body pairs based on their bounding boxes and will insert them into a queue for the narrowphase). If you make this
    // buffer too small the queue will fill up and the broad phase jobs will start to do narrow phase work. This is
    // slightly less efficient.
    static const unsigned int cMaxBodyPairs = 65536;

    // This is the maximum size of the contact constraint buffer. If more contacts (collisions between bodies) are
    // detected than this number then these contacts will be ignored and bodies will start interpenetrating / fall
    // through the world.
    static const unsigned int cMaxContactConstraints = 10240;

    namespace
    {
        LockingPolicy detectLockingPolicy()
        {
            if (Settings::physics().mAsyncNumThreads < 1)
                return LockingPolicy::NoLocks;

            return LockingPolicy::AllowSharedLocks;
        }

        unsigned getNumThreads(LockingPolicy lockingPolicy)
        {
            switch (lockingPolicy)
            {
                case LockingPolicy::NoLocks:
                    return 0;
                case LockingPolicy::AllowSharedLocks:
                    return static_cast<unsigned>(
                        std::clamp<int>(Settings::physics().mAsyncNumThreads, 0, std::thread::hardware_concurrency()));
            }

            throw std::runtime_error("Unsupported LockingPolicy: "
                + std::to_string(static_cast<std::underlying_type_t<LockingPolicy>>(lockingPolicy)));
        }
    }

    PhysicsSystem::PhysicsSystem(Resource::ResourceSystem* resourceSystem, osg::ref_ptr<osg::Group> parentNode)
        : mResourceSystem(resourceSystem)
        , mDebugDrawEnabled(false)
        , mTimeAccum(0.0f)
        , mProjectileId(0)
        , mWaterHeight(0)
        , mWaterEnabled(false)
        , mParentNode(std::move(parentNode))
        , mPhysicsDt(1.f / 60.f)
    {
        // NOTE: Jolt initialization (RegisterDefaultAllocator, Factory, RegisterTypes) is done
        // in main() before any other code runs, because Jolt types like JPH::Ref<> are used
        // in headers that get included early.

        // Set up trace and assert callbacks
        JPH::Trace = TraceImpl;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = AssertFailedImpl;)

        // Create Jolt filter/listener objects
        mContactListener = std::make_unique<JoltContactListener>();
        mBPLayerInterface = std::make_unique<JoltBPLayerInterface>();
        mObjectVsBPLayerFilter = std::make_unique<JoltObjectVsBroadPhaseLayerFilter>();
        mObjectVsObjectLayerFilter = std::make_unique<JoltObjectLayerPairFilter>();

        // Now we can safely create the shape manager (it uses Jolt types internally)
        mShapeManager = std::make_unique<Resource::PhysicsShapeManager>(resourceSystem->getVFS(),
            resourceSystem->getSceneManager(), resourceSystem->getNifFileManager(), Settings::cells().mCacheExpiryDelay);
        mResourceSystem->addResourceManager(mShapeManager.get());

        // Mark this as main thread for profiling
        JPH_PROFILE_START("Main");

        // Check if a user decided to override a physics system FPS
        if (const char* env = getenv("OPENMW_PHYSICS_FPS"))
        {
            if (const auto physFramerate = Misc::StringUtils::toNumeric<float>(env);
                physFramerate.has_value() && *physFramerate > 0)
            {
                mPhysicsDt = 1.f / *physFramerate;
                Log(Debug::Warning) << "Warning: using custom physics framerate (" << *physFramerate << " FPS).";
            }
        }

        // We need a temp allocator for temporary allocations during the physics update.
        // Pre-allocating 25 MB to avoid having to do allocations during the physics update.
        mMemoryAllocator = std::make_unique<JPH::TempAllocatorImpl>(25 * 1024 * 1024);

        // Now we can create the actual physics system.
        // NOTE: Must be created after RegisterTypes() is called, hence unique_ptr
        mPhysicsSystem = std::make_unique<JPH::PhysicsSystem>();
        mPhysicsSystem->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints, *mBPLayerInterface,
            *mObjectVsBPLayerFilter, *mObjectVsObjectLayerFilter);
        mPhysicsSystem->SetContactListener(mContactListener.get());
        mPhysicsSystem->SetGravity(JPH::Vec3(0, 0, -Constants::GravityConst * Constants::UnitsPerMeter));

        // Debug helper
        mJoltDebugDrawer = std::make_unique<MWRender::JoltDebugDrawer>(mParentNode, mPhysicsSystem.get(), mDebugDrawEnabled);

        // Detect number of wanted async threads, if 0 then use single threaded job system
        unsigned numThreads = getNumThreads(detectLockingPolicy());
        if (numThreads > 0)
        {
            mPhysicsJobSystem
                = std::make_unique<JPH::JobSystemThreadPool>(cMaxPhysicsJobs, cMaxPhysicsBarriers, numThreads);
        }
        else
        {
            mPhysicsJobSystem = std::make_unique<JPH::JobSystemSingleThreaded>(cMaxPhysicsJobs);
        }

        // Create a job scheduler responsible for simulating the game world (actors etc)
        mTaskScheduler = std::make_unique<PhysicsTaskScheduler>(
            mPhysicsDt, mPhysicsSystem.get(), mJoltDebugDrawer.get(), mPhysicsJobSystem.get());

        // Load collision shape configuration for dynamic objects
        mCollisionShapeConfig = std::make_unique<CollisionShapeConfig>();
        mCollisionShapeConfig->load(resourceSystem->getVFS(), "collision-shapes.yaml");
    }

    PhysicsSystem::~PhysicsSystem()
    {
        mBrutalRagdolls.clear();
        mResourceSystem->removeResourceManager(mShapeManager.get());
        mWaterInstance.reset();
        mTaskScheduler->releaseSharedStates();
        mRagdolls.clear();
        mDynamicObjects.clear();
        mHeightFields.clear();
        mObjects.clear();
        mActors.clear();
        mProjectiles.clear();

        // Pre-emptive end jobs system
        mPhysicsJobSystem.reset();

        // Signal to end profiling
        JPH_PROFILE_END();

        // NOTE: We do NOT call UnregisterTypes() or delete Factory::sInstance here.
        // The Jolt global state (allocator, factory, types) is initialized in main()
        // and should persist for the entire application lifetime. Destroying it here
        // would cause crashes if any Jolt-related cleanup happens after PhysicsSystem
        // is destroyed (e.g., during exception handling or in other destructors).
    }

    Resource::PhysicsShapeManager* PhysicsSystem::getShapeManager()
    {
        return mShapeManager.get();
    }

    bool PhysicsSystem::toggleDebugRendering()
    {
        mDebugDrawEnabled = !mDebugDrawEnabled;

        mJoltDebugDrawer->setDebugMode(mDebugDrawEnabled);
        return mDebugDrawEnabled;
    }

    void PhysicsSystem::markAsNonSolid(const MWWorld::ConstPtr& ptr)
    {
        ObjectMap::iterator found = mObjects.find(ptr.mRef);
        if (found == mObjects.end())
            return;

        found->second->setSolid(false);
    }

    bool PhysicsSystem::isOnSolidGround(const MWWorld::Ptr& actor) const
    {
        const Actor* physactor = getActor(actor);
        if (!physactor || !physactor->getOnGround() || !physactor->getCollisionMode())
            return false;

        const auto obj = physactor->getStandingOnPtr();
        if (obj.isEmpty())
            return true; // assume standing on terrain (which is a non-object, so not collision tracked)

        ObjectMap::const_iterator foundObj = mObjects.find(obj.mRef);
        if (foundObj == mObjects.end())
            return false;

        if (!foundObj->second->isSolid())
            return false;

        return true;
    }

    // ## VR_PATCH BEGIN
    // VR still needs getHitContact for realistic combat
    std::pair<MWWorld::Ptr, osg::Vec3f> PhysicsSystem::getHitContact(const MWWorld::ConstPtr& actor,
        const osg::Vec3f& origin, const osg::Quat& orient, float queryDistance)
    {
        const int hitmask = Layers::WORLD | Layers::DOOR | Layers::HEIGHTMAP | Layers::ACTOR;
        const MWWorld::Store<ESM::GameSetting>& store
            = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>();
        const float halfXY = osg::DegreesToRadians(store.find("fCombatAngleXY")->mValue.getFloat() * 0.5f);
        const float halfZ = osg::DegreesToRadians(store.find("fCombatAngleZ")->mValue.getFloat() * 0.5f);
        const std::array<std::pair<float, float>, 9> samples{ { { 0.f, 0.f }, { -halfXY, 0.f },
            { halfXY, 0.f }, { 0.f, -halfZ }, { 0.f, halfZ }, { -halfXY, -halfZ }, { -halfXY, halfZ },
            { halfXY, -halfZ }, { halfXY, halfZ } } };

        RayCastingResult closest;
        for (const auto& [yaw, pitch] : samples)
        {
            const osg::Quat spread
                = osg::Quat(yaw, osg::Vec3(0.f, 0.f, 1.f)) * osg::Quat(pitch, osg::Vec3(1.f, 0.f, 0.f));
            const osg::Vec3f endpoint = origin + orient * spread * osg::Vec3f(0.f, queryDistance, 0.f);
            RayCastingResult result = castRay(origin, endpoint, { actor }, {}, hitmask, Layers::ACTOR);
            const float distance = (result.mHitPos - origin).length2();
            if (result.mHit && (!closest.mHit || distance < (closest.mHitPos - origin).length2()))
                closest = result;
        }

        if (!closest.mHit)
            return { MWWorld::Ptr(), osg::Vec3f() };
        reportCollision(closest.mHitPos, closest.mHitNormal);
        return { closest.mHitObject, closest.mHitPos };
    }
    // ## VR_PATCH END

    RayCastingResult PhysicsSystem::castRay(const osg::Vec3f& from, const osg::Vec3f& to,
        const std::vector<MWWorld::ConstPtr>& ignore, const std::vector<MWWorld::Ptr>& targets, int mask,
        int group) const
    {
        if (from == to)
        {
            RayCastingResult result;
            result.mHit = false;
            return result;
        }

        JoltTargetBodiesFilter bodyFilter;

        for (const auto& ptr : ignore)
        {
            if (!ptr.isEmpty())
            {
                if (const auto found = mBrutalRagdolls.find(ptr.mRef); found != mBrutalRagdolls.end())
                    for (const auto id : found->second->getCollisionBodies()) bodyFilter.IgnoreBody(id);
                const Actor* actor = getActor(ptr);
                if (actor)
                    bodyFilter.IgnoreBody(actor->getPhysicsBody());
                else
                {
                    const Object* object = getObject(ptr);
                    if (object)
                        bodyFilter.IgnoreBody(object->getPhysicsBody());
                }
            }
        }

        if (!targets.empty())
        {
            for (const MWWorld::Ptr& target : targets)
            {
                if (const auto found = mBrutalRagdolls.find(target.mRef); found != mBrutalRagdolls.end())
                    for (const auto id : found->second->getCollisionBodies()) bodyFilter.PushTarget(id);
                const Actor* actor = getActor(target);
                if (actor)
                    bodyFilter.PushTarget(actor->getPhysicsBody());
            }
        }

        JPH::RVec3 rayOrigin = Misc::Convert::toJolt<JPH::RVec3>(from);
        auto diff = to - from;
        JPH::RRayCast ray(rayOrigin, Misc::Convert::toJolt<JPH::Vec3>(diff));

        // Filter out layers
        // TODO: restore collision group (if group == 0xff then all layers?)
        // callback.m_collisionFilterGroup = group;
        auto broadphaseLayerFilter = getRaycastBroadPhaseLayerFilter(mask);
        MaskedObjectLayerFilter objectLayerFilter(mask);

        // Cast ray and return closest hit
        JPH::RayCastResult ioHit;
        const bool didRayHit = mPhysicsSystem->GetNarrowPhaseQuery().CastRay(
            ray, ioHit, broadphaseLayerFilter, objectLayerFilter, bodyFilter);

        RayCastingResult result;
        result.mHit = didRayHit;
        if (result.mHit)
        {
            auto outPosition = ray.GetPointOnRay(ioHit.mFraction);
            result.mHitPos = Misc::Convert::toOsg(outPosition);
            JPH::BodyLockRead lock(mPhysicsSystem->GetBodyLockInterface(), ioHit.mBodyID);
            if (lock.Succeeded())
            {
                const JPH::Body& hitBody = lock.GetBody();
                result.mHitNormal
                    = Misc::Convert::toOsg(hitBody.GetWorldSpaceSurfaceNormal(ioHit.mSubShapeID2, outPosition));

                // Check UserData is non-zero before converting (it's set to 0 when object is being destroyed)
                uintptr_t userData = hitBody.GetUserData();
                if (userData != 0)
                {
                    PtrHolder* ptrHolder = Misc::Convert::toPointerFromUserData<PtrHolder>(userData);
                    if (ptrHolder)
                        result.mHitObject = ptrHolder->getPtr();
                }
            }
        }
        return result;
    }

    RayCastingResult PhysicsSystem::castSphere(
        const osg::Vec3f& from, const osg::Vec3f& to, float radius, int mask, int group) const
    {
        JPH::SphereShape sphere(radius);
        sphere.SetEmbedded();

        JPH::RMat44 transFrom = JPH::RMat44::sIdentity();
        transFrom.SetTranslation(Misc::Convert::toJolt<JPH::RVec3>(from));

        JPH::ShapeCastSettings settings;
        settings.mUseShrunkenShapeAndConvexRadius = true;
        settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
        settings.mBackFaceModeConvex = JPH::EBackFaceMode::IgnoreBackFaces;

        JPH::Vec3 scale = JPH::Vec3::sReplicate(1.0f);
        JPH::RShapeCast shapeCast(&sphere, scale, transFrom, Misc::Convert::toJolt<JPH::Vec3>(to - from));

        // Filter out layers
        // TODO: restore - collision group (if group == 0xff then all layers?)
        // callback.m_collisionFilterGroup = group;
        auto broadphaseLayerFilter = getRaycastBroadPhaseLayerFilter(mask);
        MaskedObjectLayerFilter objectLayerFilter(mask);

        ClosestConvexResultCallback callback(transFrom.GetTranslation());
        mPhysicsSystem->GetNarrowPhaseQuery().CastShape(
            shapeCast, settings, transFrom.GetTranslation(), callback, broadphaseLayerFilter, objectLayerFilter);

        RayCastingResult result;
        result.mHit = callback.hasHit();
        if (result.mHit)
        {
            result.mHitPos = Misc::Convert::toOsg(callback.mHitPointWorld);
            result.mHitNormal = Misc::Convert::toOsg(callback.mHitNormalWorld);
            if (auto* ptrHolder = static_cast<PtrHolder*>(mTaskScheduler->getUserPointer(callback.mHitCollisionObject)))
                result.mHitObject = ptrHolder->getPtr();
        }
        return result;
    }

    bool PhysicsSystem::getLineOfSight(const MWWorld::ConstPtr& actor1, const MWWorld::ConstPtr& actor2) const
    {
        if (actor1 == actor2)
            return true;

        const auto it1 = mActors.find(actor1.mRef);
        const auto it2 = mActors.find(actor2.mRef);
        if (it1 == mActors.end() || it2 == mActors.end())
            return false;

        return mTaskScheduler->getLineOfSight(it1->second, it2->second);
    }

    bool PhysicsSystem::isOnGround(const MWWorld::Ptr& actor)
    {
        Actor* physactor = getActor(actor);
        return physactor && physactor->getOnGround() && physactor->getCollisionMode();
    }

    bool PhysicsSystem::canMoveToWaterSurface(const MWWorld::ConstPtr& actor, const float waterlevel)
    {
        const auto* physactor = getActor(actor);
        return physactor && physactor->canMoveToWaterSurface(waterlevel, mPhysicsSystem.get());
    }

    osg::Vec3f PhysicsSystem::getHalfExtents(const MWWorld::ConstPtr& actor) const
    {
        const Actor* physactor = getActor(actor);
        if (physactor)
            return physactor->getHalfExtents();
        else
            return osg::Vec3f();
    }

    osg::Vec3f PhysicsSystem::getOriginalHalfExtents(const MWWorld::ConstPtr& actor) const
    {
        if (const Actor* physactor = getActor(actor))
            return physactor->getOriginalHalfExtents();
        else
            return osg::Vec3f();
    }

    osg::Vec3f PhysicsSystem::getRenderingHalfExtents(const MWWorld::ConstPtr& actor) const
    {
        const Actor* physactor = getActor(actor);
        if (physactor)
            return physactor->getRenderingHalfExtents();
        else
            return osg::Vec3f();
    }

    osg::BoundingBox PhysicsSystem::getBoundingBox(const MWWorld::ConstPtr& object) const
    {
        const Object* physobject = getObject(object);
        if (!physobject)
            return osg::BoundingBox();

        JPH::BodyID bodyId = physobject->getPhysicsBody();
        if (bodyId.IsInvalid())
            return osg::BoundingBox();

        JPH::BodyLockRead lock(mPhysicsSystem->GetBodyLockInterface(), bodyId);
        if (!lock.Succeeded())
            return osg::BoundingBox();

        const JPH::Body& body = lock.GetBody();

        // Get body world space translation + shape local space bound
        osg::Vec3f translation = Misc::Convert::toOsg(body.GetCenterOfMassTransform().GetTranslation());
        JPH::AABox bounds = body.GetShape()->GetLocalBounds();

        // Convert local space bounds to world space
        osg::Vec3f min = osg::Vec3f(bounds.mMin.GetX(), bounds.mMin.GetY(), bounds.mMin.GetZ()) + translation;
        osg::Vec3f max = osg::Vec3f(bounds.mMax.GetX(), bounds.mMax.GetY(), bounds.mMax.GetZ()) + translation;

        return osg::BoundingBox(min, max);
    }

    osg::Vec3f PhysicsSystem::getCollisionObjectPosition(const MWWorld::ConstPtr& actor) const
    {
        const Actor* physactor = getActor(actor);
        if (physactor)
            return physactor->getCollisionObjectPosition();
        else
            return osg::Vec3f();
    }

    std::vector<ContactPoint> PhysicsSystem::getCollisionsPoints(
        const MWWorld::ConstPtr& ptr, int collisionGroup, int collisionMask) const
    {
        JPH::BodyID me;

        auto found = mObjects.find(ptr.mRef);
        if (found != mObjects.end())
            me = found->second->getPhysicsBody();
        else
            return {};

        if (me.IsInvalid())
            return {};

        JPH::ShapeRefC shape;
        JPH::RMat44 transform;

        // Scoped lock so we can read then discard
        {
            JPH::BodyLockRead lock(mPhysicsSystem->GetBodyLockInterface(), me);
            if (!lock.Succeeded())
                return {};

            const JPH::Body& body = lock.GetBody();
            transform = body.GetCenterOfMassTransform();
            shape = body.GetShape();
        }

        // This sets layer filters to avoid collisions with static geometry in those cases, and allows with
        // actors->actors etc if needed this is important to prevent Jolt comlaining that two triangle mesh shapes
        // cannot collide
        JPH::DefaultBroadPhaseLayerFilter broadphaseLayerFilter
            = mPhysicsSystem->GetDefaultBroadPhaseLayerFilter(collisionGroup);
        MaskedObjectLayerFilter objectLayerFilter(collisionMask);

        JPH::CollideShapeSettings settings;
        settings.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
        settings.mBackFaceMode = JPH::EBackFaceMode::IgnoreBackFaces;
        settings.mCollectFacesMode = JPH::ECollectFacesMode::NoFaces;

        // WARNING: you cannot collide mesh->mesh shapes in Jolt, so this should filter to avoid that (only collide with
        // actors etc)
        auto scale = JPH::Vec3::sReplicate(1.0f);
        ContactTestResultCallback resultCallback(mPhysicsSystem.get(), me, transform.GetTranslation());
        mPhysicsSystem->GetNarrowPhaseQuery().CollideShape(shape, scale, transform, settings, JPH::RVec3::sZero(),
            resultCallback, broadphaseLayerFilter, objectLayerFilter);
        return resultCallback.mResult;
    }

    std::vector<MWWorld::Ptr> PhysicsSystem::getCollisions(
        const MWWorld::ConstPtr& ptr, int collisionGroup, int collisionMask) const
    {
        std::vector<MWWorld::Ptr> actors;
        for (auto& [actor, point, normal] : getCollisionsPoints(ptr, collisionGroup, collisionMask))
            actors.emplace_back(actor);
        return actors;
    }

    osg::Vec3f PhysicsSystem::traceDown(const MWWorld::Ptr& ptr, const osg::Vec3f& position, float maxHeight)
    {
        ActorMap::iterator found = mActors.find(ptr.mRef);
        if (found == mActors.end())
            return ptr.getRefData().getPosition().asVec3();
        return MovementSolver::traceDown(ptr, position, found->second.get(), mPhysicsSystem.get(), maxHeight);
    }

    void PhysicsSystem::optimize()
    {
        mTaskScheduler->optimizeBroadPhase();
    }

    void PhysicsSystem::beginBatchAdd()
    {
        mTaskScheduler->beginBatchAdd();
    }

    void PhysicsSystem::endBatchAdd()
    {
        mTaskScheduler->endBatchAdd();
        // Optimize broadphase after batch additions for better query performance
        mTaskScheduler->optimizeBroadPhase();
    }

    void PhysicsSystem::queueBodyRemoval(const MWWorld::Ptr& ptr)
    {
        stopNpcRagdoll(ptr);
        // Queue the object for batch removal - the actual body removal and destruction
        // happens when flushBodyRemovals() is called
        if (auto foundObject = mObjects.find(ptr.mRef); foundObject != mObjects.end())
        {
            mAnimatedObjects.erase(foundObject->second.get());
            mPendingObjectRemovals.push_back(ptr.mRef);
        }
        else if (auto foundDynamic = mDynamicObjects.find(ptr.mRef); foundDynamic != mDynamicObjects.end())
        {
            if (mGrabbedObject == foundDynamic->second.get())
                mGrabbedObject = nullptr;
            mPendingDynamicRemovals.push_back(ptr.mRef);
        }
        else if (auto foundActor = mActors.find(ptr.mRef); foundActor != mActors.end())
        {
            mPendingActorRemovals.push_back(ptr.mRef);

            // Also check for ragdoll associated with this actor and queue it for removal
            // Ragdolls use the same key (LiveCellRefBase*) as actors
            if (auto foundRagdoll = mRagdolls.find(ptr.mRef); foundRagdoll != mRagdolls.end())
            {
                // Release grabbed ragdoll if it's being removed
                if (mGrabbedRagdoll == foundRagdoll->second.get())
                {
                    mGrabbedRagdoll = nullptr;
                    mGrabbedRagdollBodyIndex = -1;
                }
                mPendingRagdollRemovals.push_back(ptr.mRef);
            }
        }
    }

    void PhysicsSystem::queueHeightFieldRemoval(int x, int y)
    {
        auto key = std::make_pair(x, y);
        if (mHeightFields.find(key) != mHeightFields.end())
        {
            mPendingHeightFieldRemovals.push_back(key);
        }
    }

    void PhysicsSystem::flushBodyRemovals()
    {
        if (mPendingObjectRemovals.empty() && mPendingDynamicRemovals.empty() && mPendingActorRemovals.empty()
            && mPendingRagdollRemovals.empty() && mPendingHeightFieldRemovals.empty())
            return;

        // Fully synchronize physics simulation before removing bodies (see remove() for explanation)
        mTaskScheduler->syncSimulation();

        // Clear any actor's mStandingOnPtr that references objects being removed.
        // After syncSimulation(), actors may have mStandingOnPtr set to these objects.
        // We must clear these references before destroying the objects to avoid dangling pointers.
        for (auto& [_, actor] : mActors)
        {
            MWWorld::Ptr standingOn = actor->getStandingOnPtr();
            if (standingOn.isEmpty())
                continue;
            const auto* ref = standingOn.mRef;
            bool isBeingRemoved = std::find(mPendingObjectRemovals.begin(), mPendingObjectRemovals.end(), ref)
                    != mPendingObjectRemovals.end()
                || std::find(mPendingDynamicRemovals.begin(), mPendingDynamicRemovals.end(), ref)
                    != mPendingDynamicRemovals.end()
                || std::find(mPendingActorRemovals.begin(), mPendingActorRemovals.end(), ref)
                    != mPendingActorRemovals.end();
            if (isBeingRemoved)
                actor->setStandingOnPtr(MWWorld::Ptr());
        }

        // Collect body IDs for batch removal. Jolt's RemoveBodies corrupts its broadphase quadtree
        // (crash inside QuadTree::RemoveBodies) if the batch contains a DUPLICATE, invalid, or
        // not-currently-added body. That can happen when the same object is queued more than once,
        // or a body was already removed by another path. So we deduplicate here and, for the actual
        // broadphase removal, only include bodies that are still added.
        JPH::BodyInterface& bodyInterface = mPhysicsSystem->GetBodyInterface();

        std::vector<JPH::BodyID> bodyIds;
        bodyIds.reserve(mPendingObjectRemovals.size() + mPendingDynamicRemovals.size() + mPendingActorRemovals.size()
            + mPendingHeightFieldRemovals.size());
        std::unordered_set<std::uint32_t> seenBodies;

        const auto collect = [&](JPH::BodyID id) {
            if (id.IsInvalid())
                return;
            if (!seenBodies.insert(id.GetIndexAndSequenceNumber()).second)
                return; // already collected this exact body
            bodyIds.push_back(id);
        };

        for (const auto* ref : mPendingObjectRemovals)
            if (auto it = mObjects.find(ref); it != mObjects.end())
                collect(it->second->getPhysicsBody());
        for (const auto* ref : mPendingDynamicRemovals)
            if (auto it = mDynamicObjects.find(ref); it != mDynamicObjects.end())
                collect(it->second->getPhysicsBody());
        for (const auto* ref : mPendingActorRemovals)
            if (auto it = mActors.find(ref); it != mActors.end())
                collect(it->second->getPhysicsBody());
        for (const auto& key : mPendingHeightFieldRemovals)
            if (auto it = mHeightFields.find(key); it != mHeightFields.end())
                collect(it->second->getPhysicsBody());

        if (!bodyIds.empty())
        {
            // Remove only bodies still present in the broadphase - removing an already-removed body
            // corrupts Jolt's quadtree.
            std::vector<JPH::BodyID> toRemove;
            toRemove.reserve(bodyIds.size());
            for (const JPH::BodyID& id : bodyIds)
                if (bodyInterface.IsAdded(id))
                    toRemove.push_back(id);
            if (!toRemove.empty())
                bodyInterface.RemoveBodies(toRemove.data(), static_cast<int>(toRemove.size()));

            // Destroy every collected body (each appears exactly once thanks to dedup).
            for (const JPH::BodyID& id : bodyIds)
                bodyInterface.DestroyBody(id);
        }

        // Mark bodies as removed so destructors don't try to remove/destroy again
        for (const auto* ref : mPendingObjectRemovals)
        {
            if (auto it = mObjects.find(ref); it != mObjects.end())
                it->second->markBodyRemoved();
        }
        for (const auto* ref : mPendingDynamicRemovals)
        {
            if (auto it = mDynamicObjects.find(ref); it != mDynamicObjects.end())
                it->second->markBodyRemoved();
        }
        for (const auto* ref : mPendingActorRemovals)
        {
            if (auto it = mActors.find(ref); it != mActors.end())
                it->second->markBodyRemoved();
        }
        for (const auto& key : mPendingHeightFieldRemovals)
        {
            if (auto it = mHeightFields.find(key); it != mHeightFields.end())
                it->second->markBodyRemoved();
        }

        // Now safely erase from maps (destructors won't try to remove/destroy again)
        for (const auto* ref : mPendingObjectRemovals)
            mObjects.erase(ref);
        for (const auto* ref : mPendingDynamicRemovals)
            mDynamicObjects.erase(ref);
        for (const auto* ref : mPendingActorRemovals)
            mActors.erase(ref);

        // Remove ragdolls associated with removed actors
        // Ragdoll bodies are managed by Jolt's Ragdoll class and removed in the destructor
        // via RemoveFromPhysicsSystem(), so we just need to erase from the map
        for (const auto* ref : mPendingRagdollRemovals)
            mRagdolls.erase(ref);

        // Erase heightfields
        for (const auto& key : mPendingHeightFieldRemovals)
            mHeightFields.erase(key);

        mPendingObjectRemovals.clear();
        mPendingDynamicRemovals.clear();
        mPendingActorRemovals.clear();
        mPendingRagdollRemovals.clear();
        mPendingHeightFieldRemovals.clear();
    }

    void PhysicsSystem::addHeightField(
        const float* heights, int x, int y, int size, int verts, float minH, float maxH, const osg::Object* holdObject)
    {
        mHeightFields[std::make_pair(x, y)]
            = std::make_unique<HeightField>(heights, x, y, size, verts, minH, maxH, holdObject, mTaskScheduler.get());
    }

    void PhysicsSystem::removeHeightField(int x, int y)
    {
        HeightFieldMap::iterator heightfield = mHeightFields.find(std::make_pair(x, y));
        if (heightfield != mHeightFields.end())
            mHeightFields.erase(heightfield);
    }

    const HeightField* PhysicsSystem::getHeightField(int x, int y) const
    {
        const auto heightField = mHeightFields.find(std::make_pair(x, y));
        if (heightField == mHeightFields.end())
            return nullptr;
        return heightField->second.get();
    }

    void PhysicsSystem::addObject(
        const MWWorld::Ptr& ptr, VFS::Path::NormalizedView mesh, osg::Quat rotation, int collisionType)
    {
        if (ptr.mRef->mData.mPhysicsPostponed)
            return;

        VFS::Path::Normalized animationMesh(mesh);
        if (ptr.getClass().useAnim())
            animationMesh = Misc::ResourceHelpers::correctActorModelPath(mesh, mResourceSystem->getVFS());
        osg::ref_ptr<Resource::PhysicsShapeInstance> shapeInstance = mShapeManager->getInstance(animationMesh);
        if (!shapeInstance || !shapeInstance->mCollisionShape)
        {
            return;
        }

        assert(!getObject(ptr));

        // Override collision type based on shape content.
        switch (shapeInstance->mVisualCollisionType)
        {
            case Resource::VisualCollisionType::None:
                break;
            case Resource::VisualCollisionType::Default:
                collisionType = Layers::VISUAL_ONLY;
                break;
            case Resource::VisualCollisionType::Camera:
                collisionType = Layers::CAMERA_ONLY;
                break;
        }

        auto obj = std::make_shared<Object>(ptr, shapeInstance, rotation, collisionType, mTaskScheduler.get());
        mObjects.emplace(ptr.mRef, obj);

        if (obj->isAnimated())
            mAnimatedObjects.emplace(obj.get(), false);
    }

    void PhysicsSystem::addDynamicObject(
        const MWWorld::Ptr& ptr, VFS::Path::NormalizedView mesh, osg::Quat rotation, float mass)
    {
        if (ptr.mRef->mData.mPhysicsPostponed)
            return;

        osg::ref_ptr<Resource::PhysicsShapeInstance> shapeInstance = mShapeManager->getInstance(mesh);
        if (!shapeInstance || !shapeInstance->mCollisionShape)
        {
            Log(Debug::Warning) << "No collision shape for dynamic object: " << ptr.getCellRef().getRefId();
            return;
        }

        assert(!getDynamicObject(ptr));

        // Determine collision shape type from configuration
        DynamicShapeType shapeType = DynamicShapeType::Box;
        if (mCollisionShapeConfig && mCollisionShapeConfig->isLoaded())
        {
            std::string refId = ptr.getCellRef().getRefId().getRefIdString();
            shapeType = mCollisionShapeConfig->getShapeType(refId);
        }

        auto obj = std::make_shared<DynamicObject>(ptr, shapeInstance, rotation, mass, mTaskScheduler.get(), this, shapeType);
        mDynamicObjects.emplace(ptr.mRef, obj);

        // Initialize water zone status for the new object
        const MWWorld::CellStore* cell = ptr.getCell();
        if (cell && cell->getCell())
        {
            float waterHeight = 0.0f;
            bool hasWater = false;
            if (cell->getCell()->isExterior() && mWaterEnabled)
            {
                waterHeight = mWaterHeight;
                hasWater = true;
            }
            else if (!cell->getCell()->isExterior() && cell->getCell()->hasWater())
            {
                waterHeight = cell->getWaterLevel();
                hasWater = true;
            }

            if (hasWater)
            {
                constexpr float waterZoneThreshold = 500.0f;
                osg::Vec3f pos = obj->getSimulationPosition();
                obj->setInWaterZone(pos.z() < (waterHeight + waterZoneThreshold));
            }
        }

    }

    void PhysicsSystem::remove(const MWWorld::Ptr& ptr)
    {
        stopNpcRagdoll(ptr);
        // IMPORTANT: Must fully synchronize physics simulation before removing objects.
        // This includes both waiting for the async job AND processing the simulation results.
        // The simulation results (in mSimulations vector) contain BodyIDs (like mStandingOn)
        // that reference bodies being removed. If we only wait for workers but don't process
        // the results, the next syncSimulation() call would try to access destroyed bodies
        // via getUserPointer() when processing those stale BodyIDs.
        mTaskScheduler->syncSimulation();

        // Clear any actor's mStandingOnPtr that references the object being removed.
        // After syncSimulation(), actors may have mStandingOnPtr set to this object.
        // We must clear these references before destroying the object to avoid dangling pointers.
        for (auto& [_, actor] : mActors)
        {
            if (actor->getStandingOnPtr() == ptr)
                actor->setStandingOnPtr(MWWorld::Ptr());
        }

        if (auto foundObject = mObjects.find(ptr.mRef); foundObject != mObjects.end())
        {
            mAnimatedObjects.erase(foundObject->second.get());
            mObjects.erase(foundObject);
        }
        else if (auto foundDynamic = mDynamicObjects.find(ptr.mRef); foundDynamic != mDynamicObjects.end())
        {
            // Release if we're grabbing this object
            if (mGrabbedObject == foundDynamic->second.get())
                mGrabbedObject = nullptr;

            mDynamicObjects.erase(foundDynamic);
        }
        else if (auto foundActor = mActors.find(ptr.mRef); foundActor != mActors.end())
        {
            mActors.erase(foundActor);
        }
    }

    void PhysicsSystem::removeProjectile(const int projectileId)
    {
        ProjectileMap::iterator foundProjectile = mProjectiles.find(projectileId);
        if (foundProjectile != mProjectiles.end())
            mProjectiles.erase(foundProjectile);
    }

    void PhysicsSystem::updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& updated)
    {
        if (auto node = mBrutalRagdolls.extract(old.mRef); !node.empty()) {
            node.mapped()->updatePtr(updated); node.key() = updated.mRef; mBrutalRagdolls.insert(std::move(node));
        }
        if (auto node = mPushRagdolls.extract(old.mRef); !node.empty()) { node.key() = updated.mRef; mPushRagdolls.insert(std::move(node)); }
        if (auto foundObject = mObjects.find(old.mRef); foundObject != mObjects.end())
            foundObject->second->updatePtr(updated);
        else if (auto foundDynamic = mDynamicObjects.find(old.mRef); foundDynamic != mDynamicObjects.end())
            foundDynamic->second->updatePtr(updated);
        else if (auto foundActor = mActors.find(old.mRef); foundActor != mActors.end())
            foundActor->second->updatePtr(updated);

        for (auto& [_, actor] : mActors)
        {
            if (actor->getStandingOnPtr() == old)
                actor->setStandingOnPtr(updated);
        }

        for (auto& [_, projectile] : mProjectiles)
        {
            if (projectile->getCaster() == old)
                projectile->setCaster(updated);
        }
    }

    Actor* PhysicsSystem::getActor(const MWWorld::Ptr& ptr)
    {
        ActorMap::iterator found = mActors.find(ptr.mRef);
        if (found != mActors.end())
            return found->second.get();
        return nullptr;
    }

    const Actor* PhysicsSystem::getActor(const MWWorld::ConstPtr& ptr) const
    {
        ActorMap::const_iterator found = mActors.find(ptr.mRef);
        if (found != mActors.end())
            return found->second.get();
        return nullptr;
    }

    const Object* PhysicsSystem::getObject(const MWWorld::ConstPtr& ptr) const
    {
        ObjectMap::const_iterator found = mObjects.find(ptr.mRef);
        if (found != mObjects.end())
            return found->second.get();
        return nullptr;
    }

    DynamicObject* PhysicsSystem::getDynamicObject(const MWWorld::Ptr& ptr)
    {
        DynamicObjectMap::iterator found = mDynamicObjects.find(ptr.mRef);
        if (found != mDynamicObjects.end())
            return found->second.get();
        return nullptr;
    }

    const DynamicObject* PhysicsSystem::getDynamicObject(const MWWorld::ConstPtr& ptr) const
    {
        DynamicObjectMap::const_iterator found = mDynamicObjects.find(ptr.mRef);
        if (found != mDynamicObjects.end())
            return found->second.get();
        return nullptr;
    }

    Projectile* PhysicsSystem::getProjectile(int projectileId) const
    {
        ProjectileMap::const_iterator found = mProjectiles.find(projectileId);
        if (found != mProjectiles.end())
            return found->second.get();
        return nullptr;
    }

    void PhysicsSystem::updateScale(const MWWorld::Ptr& ptr)
    {
        if (auto foundObject = mObjects.find(ptr.mRef); foundObject != mObjects.end())
        {
            float scale = ptr.getCellRef().getScale();
            foundObject->second->setScale(scale);
        }
        else if (auto foundActor = mActors.find(ptr.mRef); foundActor != mActors.end())
        {
            foundActor->second->updateScale();
        }
    }

    void PhysicsSystem::updateRotation(const MWWorld::Ptr& ptr, osg::Quat rotate)
    {
        if (auto foundObject = mObjects.find(ptr.mRef); foundObject != mObjects.end())
        {
            foundObject->second->setRotation(rotate);
        }
        else if (auto foundActor = mActors.find(ptr.mRef); foundActor != mActors.end())
        {
            if (!foundActor->second->isRotationallyInvariant())
            {
                foundActor->second->setRotation(rotate);
            }
        }
    }

    void PhysicsSystem::updatePosition(const MWWorld::Ptr& ptr)
    {
        if (auto foundObject = mObjects.find(ptr.mRef); foundObject != mObjects.end())
        {
            foundObject->second->updatePosition();
        }
        else if (auto foundActor = mActors.find(ptr.mRef); foundActor != mActors.end())
        {
            foundActor->second->updatePosition();
        }
    }

    void PhysicsSystem::addActor(const MWWorld::Ptr& ptr, VFS::Path::NormalizedView mesh)
    {
        VFS::Path::Normalized animationMesh
            = Misc::ResourceHelpers::correctActorModelPath(mesh, mResourceSystem->getVFS());
        osg::ref_ptr<const Resource::PhysicsShape> shape = mShapeManager->getShape(animationMesh);

        // Try to get shape from basic model as fallback for creatures
        if (!ptr.getClass().isNpc() && shape && shape->mCollisionBox.mExtents.length2() == 0)
        {
            if (animationMesh != mesh)
            {
                shape = mShapeManager->getShape(mesh);
            }
        }

        if (!shape)
            return;

        // check if Actor should spawn above water
        const MWMechanics::MagicEffects& effects = ptr.getClass().getCreatureStats(ptr).getMagicEffects();
        const bool canWaterWalk = effects.getOrDefault(ESM::MagicEffect::WaterWalking).getMagnitude() > 0;

        auto actor = std::make_shared<Actor>(
            ptr, shape, mTaskScheduler.get(), canWaterWalk, Settings::game().mActorCollisionShapeType);

        mActors.emplace(ptr.mRef, std::move(actor));
    }

    int PhysicsSystem::addProjectile(
        const MWWorld::Ptr& caster, const osg::Vec3f& position, VFS::Path::NormalizedView mesh, bool computeRadius)
    {
        osg::ref_ptr<Resource::PhysicsShapeInstance> shapeInstance = mShapeManager->getInstance(mesh);
        assert(shapeInstance);
        float radius = computeRadius ? shapeInstance->mCollisionBox.mExtents.length() / 2.f : 1.f;

        mProjectileId++;

        auto projectile = std::make_shared<Projectile>(caster, position, radius, mTaskScheduler.get(), this);
        mProjectiles.emplace(mProjectileId, std::move(projectile));

        return mProjectileId;
    }

    void PhysicsSystem::setCaster(int projectileId, const MWWorld::Ptr& caster)
    {
        const auto foundProjectile = mProjectiles.find(projectileId);
        assert(foundProjectile != mProjectiles.end());
        auto* projectile = foundProjectile->second.get();

        projectile->setCaster(caster);
    }

    bool PhysicsSystem::toggleCollisionMode()
    {
        ActorMap::iterator found = mActors.find(MWMechanics::getPlayer().mRef);
        if (found != mActors.end())
        {
            bool cmode = found->second->getCollisionMode();
            cmode = !cmode;
            found->second->enableCollisionMode(cmode);
            // NB: Collision body isn't disabled for vanilla TCL compatibility
            return cmode;
        }

        return false;
    }

    void PhysicsSystem::queueObjectMovement(const MWWorld::Ptr& ptr, const osg::Vec3f& velocity)
    {
        ActorMap::iterator found = mActors.find(ptr.mRef);
        if (found != mActors.end())
            found->second->setVelocity(velocity);
    }

    void PhysicsSystem::clearQueuedMovement()
    {
        // First, sync any pending simulation to process remaining results
        syncSimulation();

        for (const auto& [_, actor] : mActors)
        {
            actor->setVelocity(osg::Vec3f());
            actor->setInertialForce(osg::Vec3f());
        }

        // Clear all simulation buffers to prevent stale body IDs from being used
        // This is critical during cell transitions when bodies are destroyed and IDs may be recycled
        for (auto& simBuffer : mSimulations)
        {
            simBuffer.clear();
        }

        // Also clear any standing-on references that may point to destroyed bodies
        for (const auto& [_, actor] : mActors)
        {
            actor->setStandingOnPtr(MWWorld::Ptr());
        }
    }

    void PhysicsSystem::syncSimulation()
    {
        mTaskScheduler->syncSimulation();
    }

    void PhysicsSystem::prepareSimulation(bool willSimulate, std::vector<Simulation>& simulations)
    {
        assert(simulations.empty());
        simulations.reserve(mActors.size() + mProjectiles.size());
        const MWBase::World* world = MWBase::Environment::get().getWorld();
        for (const auto& [ref, physicActor] : mActors)
        {
            if (!physicActor->isActive() || physicActor->isRagdoll())
                continue;

            auto ptr = physicActor->getPtr();
            if (!ptr.getClass().isMobile(ptr))
                continue;
            float waterlevel = -std::numeric_limits<float>::max();
            const MWWorld::CellStore* cell = ptr.getCell();
            if (!cell || !cell->getCell())
                continue;
            if (cell->getCell()->hasWater())
                waterlevel = cell->getWaterLevel();

            const auto& stats = ptr.getClass().getCreatureStats(ptr);
            const MWMechanics::MagicEffects& effects = stats.getMagicEffects();

            bool waterCollision = false;
            if (cell->getCell()->hasWater() && effects.getOrDefault(ESM::MagicEffect::WaterWalking).getMagnitude())
            {
                if (physicActor->getCollisionMode()
                    || !world->isUnderwater(ptr.getCell(), ptr.getRefData().getPosition().asVec3()))
                    waterCollision = true;
            }

            physicActor->setCanWaterWalk(waterCollision);

            // Slow fall reduces fall speed by a factor of (effect magnitude / 200)
            const float slowFall
                = 1.f - std::clamp(effects.getOrDefault(ESM::MagicEffect::SlowFall).getMagnitude() * 0.005f, 0.f, 1.f);
            const bool isPlayer = ptr == world->getPlayerConstPtr();
            const bool godmode = isPlayer && world->getGodModeState();
            const bool inert = stats.isDead()
                || (!godmode && stats.getMagicEffects().getOrDefault(ESM::MagicEffect::Paralyze).getModifier() > 0);

            simulations.emplace_back(ActorSimulation{
                physicActor, ActorFrameData{ *physicActor, inert, waterCollision, slowFall, waterlevel, isPlayer } });

            // if the simulation will run, a jump request will be fulfilled. Update mechanics accordingly.
            if (willSimulate)
                handleJump(ptr);
        }
    }

    void PhysicsSystem::stepSimulation(
        float dt, bool skipSimulation, osg::Timer_t frameStart, unsigned int frameNumber, osg::Stats& stats)
    {
        // We cannot modify shapes at runtime while there is a body query going on (such as actor collision)
        // so we must update all animated objects first when we are guaranteed to not be having any physics queries
        {
            std::scoped_lock lock(mTaskScheduler->getSimulationMutex());
            for (auto& [animatedObject, changed] : mAnimatedObjects)
            {
                if (animatedObject->animateCollisionShapes())
                {
                    assert(mObjects.find(animatedObject->getPtr().mRef) != mObjects.end());
                    changed = true;
                }
                else
                {
                    changed = false;
                }
            }
        }

        for (auto& [_, object] : mObjects)
            object->resetCollisions();

        mTimeAccum += dt;
        mTimeAccumJolt += dt;

        if (skipSimulation)
        {
            mTaskScheduler->resetSimulation(mActors);
        }
        else
        {
            std::vector<Simulation>& simulations = mSimulations[mSimulationsCounter++ % mSimulations.size()];
            prepareSimulation(mTimeAccum >= mPhysicsDt, simulations);

            // Runs world simulation for required steps, modifies mTimeAccum
            mTaskScheduler->applyQueuedMovements(mTimeAccum, simulations, frameStart, frameNumber, stats);
        }

        // Synchronize/commit all transform updates for actors and objects
        updatePtrHolders();

        // Gravity constant for buoyancy calculations (used inside physics loop below)
        const float gravity = Constants::GravityConst * Constants::UnitsPerMeter;

        // Push dynamic objects that actors are walking into
        pushDynamicObjectsFromActors();

        // Run dynamic body sim, broadphase updates etc
        // IMPORTANT: Buoyancy must be applied INSIDE this loop, once per physics substep,
        // to ensure buoyancy and gravity are balanced correctly regardless of frame rate.
        // Cap the number of catch-up steps per frame. Without a cap a slow frame (e.g. a large battle) queues
        // several Jolt steps for the next frame, which makes that frame slower still, and the frame rate collapses.
        // Dropping the excess time only slows down loose dynamic objects while the game is below the cap's rate.
        constexpr int maxJoltStepsPerFrame = 3;
        mTimeAccumJolt = std::min(mTimeAccumJolt, mPhysicsDt * maxJoltStepsPerFrame);
        while (mTimeAccumJolt >= mPhysicsDt)
        {
            mTimeAccumJolt -= mPhysicsDt;

            // Apply buoyancy forces before each physics step
            // This ensures buoyancy is applied at the same rate as gravity
            const bool buoyancyEnabled = Settings::physics().mEnableBuoyancy;
            for (auto& [_, dynObj] : mDynamicObjects)
            {
                dynObj->updateCollisionExclusions();
                if (!buoyancyEnabled || !dynObj->isInWaterZone())
                    continue;

                MWWorld::Ptr ptr = dynObj->getPtr();
                if (ptr.isEmpty())
                    continue;

                const MWWorld::CellStore* cell = ptr.getCell();
                if (!cell || !cell->getCell())
                    continue;

                float waterHeight = 0.0f;
                bool applyBuoyancy = false;

                if (cell->getCell()->isExterior())
                {
                    if (mWaterEnabled)
                    {
                        waterHeight = mWaterHeight;
                        applyBuoyancy = true;
                    }
                }
                else
                {
                    if (cell->getCell()->hasWater())
                    {
                        waterHeight = cell->getWaterLevel();
                        applyBuoyancy = true;
                    }
                }

                if (applyBuoyancy)
                {
                    dynObj->updateBuoyancy(waterHeight, gravity, mPhysicsDt);
                }
            }

            for (auto& [key, body] : mBrutalRagdolls) body->beforeStep(mPhysicsDt);
            mPhysicsSystem->Update(mPhysicsDt, cCollisionSteps, mMemoryAllocator.get(), mPhysicsJobSystem.get());
        }

        // Update water zone tracking for dynamic objects (once per frame, not per substep)
        // This updates which objects need buoyancy checks based on their current position
        updateDynamicObjectWaterZones();

#ifdef JPH_PROFILE_ENABLED
        JPH_PROFILE_NEXTFRAME();
#endif
    }

    void PhysicsSystem::updatePtrHolders()
    {
        for (auto& [_, object] : mObjects)
            object->commitPositionChange();

        // NOTE: disabled as last step in simulation sets actor positions if changed
        // TODO: evaluate if this is needed at all, if not, remove
        // for (auto& [_, actor] : mActors)
        //     actor->updateCollisionObjectPosition();
    }

    void PhysicsSystem::moveActors()
    {
        auto* player = getActor(MWMechanics::getPlayer());
        const auto world = MWBase::Environment::get().getWorld();

        // IMPORTANT: Move player FIRST, before collecting other actors.
        // Player movement can trigger cell transitions which destroy actors from unloaded cells.
        // If we collect actor Ptrs before moving the player, those Ptrs become dangling after
        // the cell transition, causing crashes when we try to move them.
        if (player != nullptr)
            world->moveObject(player->getPtr(), player->getSimulationPosition(), false, false);

        // Now collect and move other actors. After the player moved (and possibly triggered a
        // cell transition), mActors contains only actors from currently active cells.
        mActorsPositions.clear();
        if (!mActors.empty())
            mActorsPositions.reserve(mActors.size() - 1);
        for (const auto& [ptr, physicActor] : mActors)
        {
            if (physicActor.get() == player || physicActor->isRagdoll())
                continue;
            mActorsPositions.emplace_back(physicActor->getPtr(), physicActor->getSimulationPosition());
        }

        for (const auto& [ptr, pos] : mActorsPositions)
            world->moveObject(ptr, pos, false, false);
    }

    void PhysicsSystem::applyObjectImpulse(const MWWorld::Ptr& ptr, const osg::Vec3f& impulse, float mass)
    {
        if (!Settings::game().mBrutalDynamicProps) return;
        if (!std::isfinite(mass) || mass <= 0.f) throw std::runtime_error("Invalid dynamic prop mass");
        mTaskScheduler->syncSimulation();
        if (!mDynamicObjects.contains(ptr.mRef))
        {
            if (const auto found = mObjects.find(ptr.mRef); found != mObjects.end())
            {
                if (found->second->isAnimated() || !found->second->isSolid())
                    throw std::runtime_error("Dynamic prop requires a solid, non-animated collision shape");
            }
            addDynamicObject(ptr, ptr.getClass().getCorrectedModel(ptr),
                ptr.getRefData().getBaseNode()->getAttitude(), mass);
        }
        const auto found = mDynamicObjects.find(ptr.mRef);
        if (found == mDynamicObjects.end()) throw std::runtime_error("Dynamic prop has no collision shape");
        if (const auto original = mObjects.find(ptr.mRef); original != mObjects.end())
        {
            mAnimatedObjects.erase(original->second.get());
            mObjects.erase(original);
        }
        found->second->applyImpulse(impulse);
    }

    void PhysicsSystem::addReleasedEquipment(const MWWorld::Ptr& ptr)
    {
        if (!Settings::game().mBrutalEquipmentPhysics || mDynamicObjects.contains(ptr.mRef)) return;
        const auto& release = ptr.getRefData().getReleasedEquipment();
        if (!release || !release->valid()) throw std::runtime_error("Invalid released equipment descriptor");
        JPH::ConvexHullShapeSettings hull;
        for (const auto& drawable : release->mDrawables)
        {
            const osg::Matrixf residual(drawable.mTransform.data());
            for (const auto& vertex : drawable.mVertices)
                hull.mPoints.push_back(Misc::Convert::toJolt<JPH::Vec3>(
                    osg::Vec3f(vertex[0], vertex[1], vertex[2]) * residual));
        }
        hull.mMaxConvexRadius = .05f;
        auto result = hull.Create();
        if (result.HasError()) throw std::runtime_error(result.GetError().c_str());
        auto shape = osg::ref_ptr<Resource::PhysicsShape>(new Resource::PhysicsShape);
        shape->mCollisionShape = result.Get();
        shape->mVisualCollisionType = Resource::VisualCollisionType::Default;
        mTaskScheduler->syncSimulation();
        auto object = std::make_shared<DynamicObject>(ptr, Resource::makeInstance(shape),
            ptr.getRefData().getBaseNode()->getAttitude(), release->mMass,
            mTaskScheduler.get(), this, DynamicShapeType::Box, true);
        mDynamicObjects.emplace(ptr.mRef, std::move(object));
    }

    void PhysicsSystem::initializeReleasedEquipment(const MWWorld::Ptr& item, const MWWorld::Ptr& actor,
        const osg::Vec3f& linear, const osg::Vec3f& angular)
    {
        const auto object = mDynamicObjects.find(item.mRef);
        if (object == mDynamicObjects.end() || !mBrutalRagdolls.contains(actor.mRef))
            throw std::runtime_error("Released equipment or owner body is missing");
        mTaskScheduler->syncSimulation();
        object->second->ignoreOverlappingBodies(mBrutalRagdolls.at(actor.mRef)->getEquipmentGripBodies());
        object->second->setLinearVelocity(linear);
        object->second->setAngularVelocity(angular);
    }

    bool PhysicsSystem::getRagdollEquipmentFrame(const MWWorld::Ptr& actor, bool left,
        osg::Matrixf& frame, osg::Vec3f& linear, osg::Vec3f& angular) const
    {
        const auto found = mBrutalRagdolls.find(actor.mRef);
        return found != mBrutalRagdolls.end() && found->second->getEquipmentFrame(left, frame, linear, angular);
    }

    std::vector<osg::Vec3f> PhysicsSystem::getReleasedEquipmentState(const MWWorld::Ptr& item) const
    {
        const auto found = mDynamicObjects.find(item.mRef);
        if (found == mDynamicObjects.end()) return {};
        mTaskScheduler->syncSimulation();
        const auto& object = *found->second;
        JPH::Vec3 linear, angular;
        mPhysicsSystem->GetBodyInterface().GetLinearAndAngularVelocity(object.getPhysicsBody(), linear, angular);
        JPH::BodyIDVector bodies;
        mPhysicsSystem->GetBodies(bodies);
        unsigned worldContacts = 0, ragdollContacts = 0;
        for (const auto id : bodies)
            if (mPhysicsSystem->WereBodiesInContact(object.getPhysicsBody(), id))
            {
                bool ragdoll = false;
                for (const auto& [ref, body] : mBrutalRagdolls)
                    if (const auto& parts = body->getCollisionBodies(); std::find(parts.begin(),parts.end(),id) != parts.end())
                    { ragdoll = true; break; }
                if (ragdoll) ++ragdollContacts; else ++worldContacts;
            }
        return { object.getSimulationPosition(), Misc::Convert::toOsg(linear), Misc::Convert::toOsg(angular),
            osg::Vec3f(object.isActive() ? 1.f : 0.f, float(worldContacts+ragdollContacts), float(object.ignoredBodyCount())),
            osg::Vec3f(float(worldContacts),float(ragdollContacts),0.f) };
    }

    void PhysicsSystem::moveDynamicObjects()
    {
        mTaskScheduler->syncSimulation();
        struct Update { MWWorld::Ptr ptr; osg::Vec3f position; osg::Quat rotation; };
        std::vector<Update> updates;
        updates.reserve(mDynamicObjects.size());
        for (const auto& [ref, object] : mDynamicObjects)
            updates.push_back({object->getPtr(),object->getSimulationPosition(),object->getSimulationRotation()});
        const auto world = MWBase::Environment::get().getWorld();
        for (const auto& update : updates)
            if (update.ptr.isInCell() && update.ptr.getRefData().getBaseNode())
                world->syncDynamicObject(update.ptr,update.position,update.rotation,update.position,update.rotation,true);
    }

    void PhysicsSystem::debugDraw()
    {
        if (mDebugDrawEnabled)
            mTaskScheduler->debugDraw();
    }

    bool PhysicsSystem::isActorStandingOn(const MWWorld::Ptr& actor, const MWWorld::ConstPtr& object) const
    {
        const auto physActor = mActors.find(actor.mRef);
        if (physActor != mActors.end())
            return physActor->second->getStandingOnPtr() == object;
        return false;
    }

    void PhysicsSystem::getActorsStandingOn(const MWWorld::ConstPtr& object, std::vector<MWWorld::Ptr>& out) const
    {
        for (const auto& [_, actor] : mActors)
        {
            if (actor->getStandingOnPtr() == object)
                out.emplace_back(actor->getPtr());
        }
    }

    bool PhysicsSystem::isObjectCollidingWith(const MWWorld::ConstPtr& object, ScriptedCollisionType type) const
    {
        auto found = mObjects.find(object.mRef);
        if (found != mObjects.end())
            return found->second->collidedWith(type);
        return false;
    }

    void PhysicsSystem::getActorsCollidingWith(const MWWorld::ConstPtr& object, std::vector<MWWorld::Ptr>& out) const
    {
        std::vector<MWWorld::Ptr> collisions = getCollisions(object, Layers::WORLD, Layers::ACTOR);
        out.insert(out.end(), collisions.begin(), collisions.end());
    }

    void PhysicsSystem::disableWater()
    {
        if (mWaterEnabled)
        {
            mWaterEnabled = false;
            updateWater();
        }
    }

    void PhysicsSystem::enableWater(float height)
    {
        if (!mWaterEnabled || mWaterHeight != height)
        {
            mWaterEnabled = true;
            mWaterHeight = height;
            updateWater();
        }
    }

    void PhysicsSystem::setWaterHeight(float height)
    {
        if (mWaterHeight != height)
        {
            mWaterHeight = height;
            updateWater();
        }
    }

    void PhysicsSystem::updateWater()
    {
        if (!mWaterEnabled)
        {
            mWaterInstance.reset();
            // Clear water zone flags for all dynamic objects
            for (auto& [_, dynObj] : mDynamicObjects)
                dynObj->setInWaterZone(false);
            return;
        }

        mWaterInstance = std::make_unique<MWWater>(mTaskScheduler.get(), mWaterHeight);

        // Update water zone flags for all dynamic objects
        updateDynamicObjectWaterZones();
    }

    void PhysicsSystem::updateDynamicObjectWaterZones()
    {
        // Water zone threshold: objects within this distance above water get buoyancy checks
        // This ensures objects falling toward water start buoyancy before fully submerging
        constexpr float waterZoneThreshold = 500.0f;

        for (auto& [_, dynObj] : mDynamicObjects)
        {
            MWWorld::Ptr ptr = dynObj->getPtr();
            if (ptr.isEmpty())
            {
                dynObj->setInWaterZone(false);
                continue;
            }

            const MWWorld::CellStore* cell = ptr.getCell();
            if (!cell || !cell->getCell())
            {
                dynObj->setInWaterZone(false);
                continue;
            }

            float waterHeight = 0.0f;
            bool hasWater = false;

            if (cell->getCell()->isExterior())
            {
                if (mWaterEnabled)
                {
                    waterHeight = mWaterHeight;
                    hasWater = true;
                }
            }
            else
            {
                if (cell->getCell()->hasWater())
                {
                    waterHeight = cell->getWaterLevel();
                    hasWater = true;
                }
            }

            if (!hasWater)
            {
                dynObj->setInWaterZone(false);
                continue;
            }

            // Check if object is near water (below water + threshold above)
            osg::Vec3f pos = dynObj->getSimulationPosition();
            bool inZone = pos.z() < (waterHeight + waterZoneThreshold);
            dynObj->setInWaterZone(inZone);
        }
    }

    bool PhysicsSystem::isAreaOccupiedByOtherActor(
        const MWWorld::LiveCellRefBase* actor, const osg::Vec3f& position, float radius) const
    {
        // Build ignored body ID for the actor
        JPH::BodyID ignoredBody;
        if (actor != nullptr)
        {
            if (const auto it = mActors.find(actor); it != mActors.end())
                ignoredBody = it->second->getPhysicsBody();
        }

        // Define a tiny private class here as its not used anywhere else
        class AreaActorCollector : public JPH::CollideShapeBodyCollector
        {
        public:
            AreaActorCollector(JPH::BodyID ignoredBody)
                : mIgnoredBody(ignoredBody)
            {
            }

            virtual void AddHit(const JPH::BodyID& inBodyID) override
            {
                // Check if we should ignore this body
                if (inBodyID == mIgnoredBody)
                    return;

                mHasHit = true;
            }

            bool hasHit() { return mHasHit; }

        private:
            JPH::BodyID mIgnoredBody;
            bool mHasHit = false;
        };

        // Setup collector and collide body AABBs by the input sphere
        AreaActorCollector collector(ignoredBody);
        JPH::DefaultBroadPhaseLayerFilter broadphaseLayerFilter
            = mPhysicsSystem->GetDefaultBroadPhaseLayerFilter(Layers::ACTOR);
        mPhysicsSystem->GetBroadPhaseQuery().CollideSphere(Misc::Convert::toJolt<JPH::Vec3>(position), radius, collector,
            broadphaseLayerFilter, JPH::SpecifiedObjectLayerFilter(Layers::ACTOR));

        return collector.hasHit();
    }

    void PhysicsSystem::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        stats.setAttribute(frameNumber, "Physics Actors", mActors.size());
        stats.setAttribute(frameNumber, "Physics Objects", mObjects.size());
        stats.setAttribute(frameNumber, "Physics Projectiles", mProjectiles.size());
        stats.setAttribute(frameNumber, "Physics HeightFields", mHeightFields.size());
    }

    void PhysicsSystem::reportCollision(const osg::Vec3f& position, const osg::Vec3f& normal)
    {
        if (mDebugDrawEnabled)
            mJoltDebugDrawer->addCollision(position, normal);
    }

    const JPH::BodyLockInterfaceLocking& PhysicsSystem::getBodyLockInterface() const
    {
        return mPhysicsSystem->GetBodyLockInterface();
    }

    const JPH::BodyInterface& PhysicsSystem::getBodyInterface() const
    {
        return mPhysicsSystem->GetBodyInterface();
    }

    ActorFrameData::ActorFrameData(
        Actor& actor, bool inert, bool waterCollision, float slowFall, float waterlevel, bool isPlayer)
        : mPosition()
        , mExternalVelocity(actor.getExternalVelocity())
        , mLocomotionVelocity(actor.getLocomotionVelocity())
        , mKineticMovement(isPlayer && Settings::game().mKineticPlayerMovement)
        , mBrutalSliding(isPlayer && actor.getPtr().getClass().getCreatureStats(actor.getPtr()).mBrutalSliding)
        , mBrutalSlideLow(isPlayer && actor.getPtr().getClass().getCreatureStats(actor.getPtr()).mBrutalSlideLow)
        , mBrutalSlideFrictionBlend(mBrutalSliding ? 1.f
              : mKineticMovement && mBrutalSlideLow
                  && actor.getPtr().getClass().getCreatureStats(actor.getPtr()).mBrutalSlidePhase == 3
              ? std::clamp(actor.getPtr().getClass().getCreatureStats(actor.getPtr()).mBrutalSlideBlend, 0.f, 1.f)
              : 0.f)
        , mIsOnGround(actor.getOnGround())
        , mIsOnSlope(actor.getOnSlope())
        , mWalkingOnWater(false)
        , mInert(inert)
        , mPhysicsBody(actor.getPhysicsBody())
        , mSwimLevel(waterlevel
              - (actor.getRenderingHalfExtents().z() * 2
                  * MWBase::Environment::get()
                        .getESMStore()
                        ->get<ESM::GameSetting>()
                        .find("fSwimHeightScale")
                        ->mValue.getFloat()))
        , mSlowFall(slowFall)
        , mRotation()
        , mMovement(actor.velocity())
        , mWaterlevel(waterlevel)
        , mHalfExtentsZ(actor.getHalfExtents().z())
        , mOldHeight(0)
        , mStuckFrames(0)
        , mFlying(MWBase::Environment::get().getWorld()->isFlying(actor.getPtr()))
        , mWasOnGround(actor.getOnGround())
        , mIsAquatic(actor.getPtr().getClass().isPureWaterCreature(actor.getPtr()))
        , mWaterCollision(waterCollision)
        , mSkipCollisionDetection(!actor.getCollisionMode())
        , mIsPlayer(isPlayer)
        , mCollisionMask(actor.getCollisionMask())
    {
    }

    WorldFrameData::WorldFrameData()
        : mIsInStorm(MWBase::Environment::get().getWorld()->isInStorm())
        , mStormDirection(MWBase::Environment::get().getWorld()->getStormDirection())
    {
    }

    LOSRequest::LOSRequest(const std::weak_ptr<Actor>& a1, const std::weak_ptr<Actor>& a2)
        : mResult(false)
        , mStale(false)
        , mAge(0)
    {
        // we use raw actor pointer pair to uniquely identify request
        // sort the pointer value in ascending order to not duplicate equivalent requests, eg. getLOS(A, B) and
        // getLOS(B, A)
        auto* raw1 = a1.lock().get();
        auto* raw2 = a2.lock().get();
        assert(raw1 != raw2);
        if (raw1 < raw2)
        {
            mActors = { a1, a2 };
            mRawActors = { raw1, raw2 };
        }
        else
        {
            mActors = { a2, a1 };
            mRawActors = { raw2, raw1 };
        }
    }

    bool operator==(const LOSRequest& lhs, const LOSRequest& rhs) noexcept
    {
        return lhs.mRawActors == rhs.mRawActors;
    }

    bool PhysicsSystem::grabObject(const osg::Vec3f& rayStart, const osg::Vec3f& rayDir, float maxDistance)
    {
        // Release any currently held object
        if (mGrabbedObject)
            releaseGrabbedObject();

        // Cast ray to find a dynamic object
        osg::Vec3f rayEnd = rayStart + rayDir * maxDistance;

        JPH::RVec3 rayOrigin = Misc::Convert::toJolt<JPH::RVec3>(rayStart);
        JPH::Vec3 rayDirection = Misc::Convert::toJolt<JPH::Vec3>(rayEnd - rayStart);
        JPH::RRayCast ray(rayOrigin, rayDirection);

        // Only collide with dynamic objects
        JPH::SpecifiedBroadPhaseLayerFilter broadphaseLayerFilter(BroadPhaseLayers::MOVING);
        JPH::SpecifiedObjectLayerFilter objectLayerFilter(Layers::DYNAMIC_WORLD);

        JPH::RayCastResult hit;
        bool didHit = mPhysicsSystem->GetNarrowPhaseQuery().CastRay(
            ray, hit, broadphaseLayerFilter, objectLayerFilter);

        if (!didHit)
            return false;

        // Get the body and verify it's a dynamic object
        DynamicObject* dynObj = nullptr;
        {
            JPH::BodyLockRead lock(mPhysicsSystem->GetBodyLockInterface(), hit.mBodyID);
            if (!lock.Succeeded())
                return false;

            const JPH::Body& body = lock.GetBody();
            if (body.GetObjectLayer() != Layers::DYNAMIC_WORLD)
                return false;

            // Get the DynamicObject from user data
            uintptr_t userData = body.GetUserData();
            if (userData == 0)
                return false;

            dynObj = reinterpret_cast<DynamicObject*>(userData);
            if (!dynObj)
                return false;

            // Verify the dynamic object is still valid by checking its ptr is not empty
            // The ptr becomes empty when the object is removed from the world
            if (dynObj->getPtr().isEmpty())
            {
                Log(Debug::Warning) << "Attempted to grab invalid dynamic object (empty ptr)";
                return false;
            }
        }
        // Lock is now released

        // Calculate grab distance (distance from ray start to hit point)
        osg::Vec3f hitPoint = rayStart + rayDir * (hit.mFraction * maxDistance);
        mGrabDistance = (hitPoint - rayStart).length();

        // Clamp grab distance to reasonable range
        mGrabDistance = std::clamp(mGrabDistance, 50.0f, maxDistance);

        mGrabbedObject = dynObj;
        mGrabTargetPosition = hitPoint;

        // Wake up the object (must be outside the body lock to avoid deadlock)
        dynObj->activate();

        Log(Debug::Verbose) << "Grabbed object: " << dynObj->getPtr().getCellRef().getRefId();

        return true;
    }

    void PhysicsSystem::releaseGrabbedObject(const osg::Vec3f& throwVelocity)
    {
        if (!mGrabbedObject)
            return;

        // Apply throw velocity if provided
        if (throwVelocity.length2() > 0.01f)
        {
            mGrabbedObject->setLinearVelocity(throwVelocity);
        }

        Log(Debug::Verbose) << "Released object: " << mGrabbedObject->getPtr().getCellRef().getRefId();

        mGrabbedObject = nullptr;
    }

    void PhysicsSystem::updateGrabbedObject(const osg::Vec3f& targetPosition)
    {
        if (!mGrabbedObject)
            return;

        // Validate grabbed object is still valid (could have been removed during cell transition)
        JPH::BodyID bodyId = mGrabbedObject->getPhysicsBody();
        if (bodyId.IsInvalid())
        {
            Log(Debug::Warning) << "Grabbed object body is invalid, releasing";
            mGrabbedObject = nullptr;
            return;
        }

        // Verify the body still exists in the physics system
        // Use a separate scope for the lock to avoid holding it during physics operations
        {
            JPH::BodyLockRead lock(mPhysicsSystem->GetBodyLockInterface(), bodyId);
            if (!lock.Succeeded())
            {
                Log(Debug::Warning) << "Grabbed object body lock failed, releasing";
                mGrabbedObject = nullptr;
                return;
            }
        }
        // Lock is now released - safe to call physics operations

        mGrabTargetPosition = targetPosition;

        // Get current position
        osg::Vec3f currentPos = mGrabbedObject->getSimulationPosition();

        // Calculate displacement to target
        osg::Vec3f displacement = mGrabTargetPosition - currentPos;
        float distance = displacement.length();

        // Movement parameters - tuned for smooth, responsive Oblivion/Skyrim feel
        // Higher values = snappier movement, lower = more floaty
        constexpr float moveSpeed = 15.0f;      // How fast the object moves toward target (units per second per unit distance)
        constexpr float maxSpeed = 1500.0f;     // Maximum velocity to prevent extreme speeds
        constexpr float snapDistance = 1.0f;    // Distance at which we consider the object "arrived"

        osg::Vec3f targetVelocity;

        if (distance < snapDistance)
        {
            // Very close - slow down proportionally to avoid overshoot
            targetVelocity = displacement * moveSpeed * 0.5f;
        }
        else
        {
            // Move toward target at speed proportional to distance
            // This creates smooth acceleration/deceleration
            osg::Vec3f direction = displacement / distance;
            float speed = std::min(distance * moveSpeed, maxSpeed);
            targetVelocity = direction * speed;
        }

        // Directly set velocity for responsive, smooth movement
        // This bypasses physics simulation lag and gives immediate response
        mGrabbedObject->setLinearVelocity(targetVelocity);

        // Zero out angular velocity to prevent spinning while held
        mGrabbedObject->setAngularVelocity(osg::Vec3f(0, 0, 0));

        // Keep the object active
        mGrabbedObject->activate();
    }

    MWWorld::Ptr PhysicsSystem::getGrabbedObject() const
    {
        if (mGrabbedObject)
            return mGrabbedObject->getPtr();
        return MWWorld::Ptr();
    }

    void PhysicsSystem::pushDynamicObjectsFromActors()
    {
        // For each actor, check if they're overlapping with dynamic objects
        // and push those objects based on the actor's velocity
        for (const auto& [_, actor] : mActors)
        {
            if (!actor->isActive())
                continue;

            osg::Vec3f actorPos = actor->getSimulationPosition();
            osg::Vec3f actorVelocity = actor->getInertialForce();

            // Also consider movement velocity
            // The actor's velocity is stored before being consumed
            float actorSpeed = actorVelocity.length();
            if (actorSpeed < 10.0f)
                continue;  // Actor not moving fast enough to push

            osg::Vec3f actorHalfExtents = actor->getHalfExtents();
            float actorRadius = std::max(actorHalfExtents.x(), actorHalfExtents.y()) + 20.0f;  // Add margin

            for (auto& [_, dynObj] : mDynamicObjects)
            {
                // Don't push grabbed object
                if (dynObj.get() == mGrabbedObject)
                    continue;

                osg::Vec3f objPos = dynObj->getSimulationPosition();
                osg::Vec3f toObject = objPos - actorPos;

                // Quick distance check (XY plane primarily)
                float distXY = std::sqrt(toObject.x() * toObject.x() + toObject.y() * toObject.y());
                float distZ = std::abs(toObject.z());

                // Check if within actor's collision cylinder
                if (distXY > actorRadius || distZ > actorHalfExtents.z() + 30.0f)
                    continue;

                // Calculate push direction (away from actor center, mostly horizontal)
                osg::Vec3f pushDir = toObject;
                pushDir.z() *= 0.3f;  // Reduce vertical component
                float pushDist = pushDir.length();
                if (pushDist < 0.1f)
                    pushDir = osg::Vec3f(1.0f, 0.0f, 0.0f);  // Default push direction
                else
                    pushDir /= pushDist;

                // Impulse strength based on actor speed and proximity
                float proximityFactor = 1.0f - (distXY / actorRadius);
                proximityFactor = std::max(proximityFactor, 0.2f);

                constexpr float basePushStrength = 100.0f;
                float impulseMagnitude = basePushStrength * proximityFactor * (actorSpeed / 100.0f);

                osg::Vec3f impulse = pushDir * impulseMagnitude;
                impulse.z() += impulseMagnitude * 0.3f;  // Add upward kick

                dynObj->activate();
                dynObj->applyImpulse(impulse);
            }
        }
    }

    void PhysicsSystem::applyMeleeHitToDynamicObjects(const osg::Vec3f& origin, const osg::Vec3f& direction,
        float reach, float attackStrength)
    {
        Log(Debug::Verbose) << "applyMeleeHitToDynamicObjects called: origin=" << origin.x() << "," << origin.y() << "," << origin.z()
                            << " reach=" << reach << " strength=" << attackStrength
                            << " dynamicObjects=" << mDynamicObjects.size();

        // Cone check parameters - realistic weapon swing arc
        constexpr float coneAngleCos = 0.707f;  // ~45 degree half-angle cone

        osg::Vec3f normalizedDir = direction;
        normalizedDir.normalize();

        for (auto& [_, dynObj] : mDynamicObjects)
        {
            // Don't hit the grabbed object
            if (dynObj.get() == mGrabbedObject)
                continue;

            osg::Vec3f objPos = dynObj->getSimulationPosition();
            osg::Vec3f toObject = objPos - origin;
            float distance = toObject.length();

            // Check if within reach (exact reach distance)
            if (distance > reach || distance < 0.1f)
                continue;

            toObject.normalize();

            // Check if within cone angle
            float dot = toObject * normalizedDir;
            if (dot < coneAngleCos)
                continue;

            // Calculate impulse based on attack strength and distance
            // Closer objects get hit harder
            float distanceFactor = 1.0f - (distance / reach);
            distanceFactor = std::max(distanceFactor, 0.2f);  // Minimum 20% power

            // Base impulse scaled by attack strength
            float mass = dynObj->getMass();
            constexpr float baseImpulse = 500.0f;
            float impulseMagnitude = baseImpulse * attackStrength * distanceFactor * std::max(1.0f, mass * 0.5f);

            // Apply impulse in the swing direction with slight upward component
            osg::Vec3f impulse = normalizedDir * impulseMagnitude;
            impulse.z() += impulseMagnitude * 0.3f;  // Slight upward kick

            // IMPORTANT: Activate the body BEFORE applying impulse
            // Otherwise sleeping bodies may ignore the impulse
            dynObj->activate();
            dynObj->applyImpulse(impulse);

            Log(Debug::Info) << "Melee hit dynamic object: " << dynObj->getPtr().getCellRef().getRefId()
                             << " impulse=" << impulseMagnitude << " mass=" << mass << " dist=" << distance;
        }
    }

    void PhysicsSystem::activateRagdoll(const MWWorld::Ptr& ptr, SceneUtil::Skeleton* skeleton,
        const osg::Vec3f& hitImpulse)
    {
        // Check if ragdoll physics is enabled in settings
        if (Settings::game().mBrutalRagdoll || !Settings::physics().mEnableRagdoll)
            return;

        if (!ptr.getClass().isActor())
            return;

        const MWWorld::LiveCellRefBase* key = ptr.getBase();

        // Check if ragdoll already exists
        if (mRagdolls.find(key) != mRagdolls.end())
            return;

        // Remove the actor's kinematic physics body first
        auto actorIt = mActors.find(key);
        if (actorIt != mActors.end())
        {
            // Disable external collision so other actors don't interact with the old body
            actorIt->second->enableCollisionBody(false);
        }

        // Enforce max ragdoll limit - remove oldest/farthest if at limit
        if (static_cast<int>(mRagdolls.size()) >= sMaxActiveRagdolls)
        {
            // Find ragdoll that's been at rest the longest
            const MWWorld::LiveCellRefBase* toRemove = nullptr;
            for (const auto& [ragdollKey, ragdoll] : mRagdolls)
            {
                if (ragdoll->isAtRest())
                {
                    toRemove = ragdollKey;
                    break;
                }
            }

            // If none at rest, remove the first one
            if (!toRemove && !mRagdolls.empty())
            {
                toRemove = mRagdolls.begin()->first;
            }

            if (toRemove)
            {
                mRagdolls.erase(toRemove);
                Log(Debug::Verbose) << "Ragdoll limit reached, removed oldest ragdoll";
            }
        }

        // Get actor transform
        osg::Vec3f position = ptr.getRefData().getPosition().asVec3();
        osg::Quat rotation = Misc::Convert::makeOsgQuat(ptr.getRefData().getPosition());
        float scale = ptr.getCellRef().getScale();

        // Create the ragdoll using the new wrapper
        auto ragdoll = std::make_shared<RagdollWrapper>(
            ptr, skeleton, position, rotation, scale,
            mPhysicsSystem.get(), mTaskScheduler.get());

        if (!ragdoll->isValid())
        {
            // Ragdoll construction failed. Restore the actor's normal (kinematic) collision body
            // that we disabled above, so the corpse still settles/blocks normally, and let the
            // caller fall back to the default death animation instead of freezing.
            if (actorIt != mActors.end())
                actorIt->second->enableCollisionBody(true);
            Log(Debug::Verbose) << "Ragdoll construction failed for " << ptr.getCellRef().getRefId()
                                << "; using default death animation";
            return;
        }

        // Apply initial impulse from the killing blow
        if (hitImpulse.length2() > 0)
        {
            ragdoll->applyRootImpulse(hitImpulse);
        }

        mRagdolls[key] = ragdoll;

        Log(Debug::Info) << "Activated ragdoll for " << ptr.getCellRef().getRefId();
    }

    void PhysicsSystem::removeRagdoll(const MWWorld::Ptr& ptr)
    {
        const MWWorld::LiveCellRefBase* key = ptr.getBase();
        auto it = mRagdolls.find(key);
        if (it != mRagdolls.end())
        {
            mRagdolls.erase(it);
            Log(Debug::Verbose) << "Removed ragdoll for " << ptr.getCellRef().getRefId();
        }
    }

    RagdollWrapper* PhysicsSystem::getRagdoll(const MWWorld::Ptr& ptr)
    {
        auto it = mRagdolls.find(ptr.getBase());
        if (it != mRagdolls.end())
            return it->second.get();
        return nullptr;
    }

    const RagdollWrapper* PhysicsSystem::getRagdoll(const MWWorld::ConstPtr& ptr) const
    {
        auto it = mRagdolls.find(ptr.getBase());
        if (it != mRagdolls.end())
            return it->second.get();
        return nullptr;
    }

    void PhysicsSystem::updateRagdolls()
    {
        for (auto& [key, ragdoll] : mRagdolls)
        {
            ragdoll->updateBoneTransforms();
        }
    }

    bool PhysicsSystem::hasRagdoll(const MWWorld::ConstPtr& ptr) const
    {
        return mRagdolls.find(ptr.getBase()) != mRagdolls.end();
    }

    bool PhysicsSystem::grabRagdoll(const osg::Vec3f& rayStart, const osg::Vec3f& rayDir, float maxDistance)
    {
        // Release any currently held object/ragdoll
        if (mGrabbedObject)
            releaseGrabbedObject();
        if (mGrabbedRagdoll)
            releaseGrabbedRagdoll();

        // Calculate ray endpoint
        osg::Vec3f rayEnd = rayStart + rayDir * maxDistance;

        // Find the closest ragdoll body to the ray
        RagdollWrapper* closestRagdoll = nullptr;
        int closestBodyIndex = -1;
        float closestDistance = maxDistance;
        osg::Vec3f closestHitPoint;

        // For each ragdoll, check each body's distance to the ray
        for (auto& [key, ragdoll] : mRagdolls)
        {
            if (!ragdoll || !ragdoll->isValid())
                continue;

            int bodyCount = ragdoll->getBodyCount();
            for (int i = 0; i < bodyCount; ++i)
            {
                osg::Vec3f bodyPos = ragdoll->getBodyPosition(i);

                // Calculate closest point on ray to body center
                osg::Vec3f rayVec = rayEnd - rayStart;
                float rayLength = rayVec.length();
                if (rayLength < 0.001f)
                    continue;

                osg::Vec3f rayNorm = rayVec / rayLength;
                osg::Vec3f toBody = bodyPos - rayStart;
                float t = toBody * rayNorm;

                // Clamp t to ray segment
                t = std::clamp(t, 0.0f, rayLength);

                // Closest point on ray
                osg::Vec3f closestPointOnRay = rayStart + rayNorm * t;
                float distToRay = (bodyPos - closestPointOnRay).length();

                // Check if this body is close enough to the ray (within ~30 units)
                constexpr float grabRadius = 30.0f;
                if (distToRay < grabRadius && t < closestDistance)
                {
                    closestDistance = t;
                    closestRagdoll = ragdoll.get();
                    closestBodyIndex = i;
                    closestHitPoint = closestPointOnRay;
                }
            }
        }

        if (!closestRagdoll || closestBodyIndex < 0)
            return false;

        // Set up the grab
        mGrabbedRagdoll = closestRagdoll;
        mGrabbedRagdollBodyIndex = closestBodyIndex;
        mGrabDistance = closestDistance;
        mGrabTargetPosition = closestHitPoint;

        // Activate the ragdoll
        closestRagdoll->activate();

        Log(Debug::Verbose) << "Grabbed ragdoll body " << closestBodyIndex
                            << " of " << closestRagdoll->getPtr().getCellRef().getRefId()
                            << " at distance " << closestDistance;

        return true;
    }

    void PhysicsSystem::releaseGrabbedRagdoll(const osg::Vec3f& throwVelocity)
    {
        if (!mGrabbedRagdoll)
            return;

        // Apply throw velocity if provided
        if (throwVelocity.length2() > 0.01f && mGrabbedRagdollBodyIndex >= 0)
        {
            mGrabbedRagdoll->setBodyVelocity(mGrabbedRagdollBodyIndex, throwVelocity);
        }

        Log(Debug::Verbose) << "Released ragdoll: " << mGrabbedRagdoll->getPtr().getCellRef().getRefId();

        mGrabbedRagdoll = nullptr;
        mGrabbedRagdollBodyIndex = -1;
    }

    void PhysicsSystem::updateGrabbedRagdoll(const osg::Vec3f& targetPosition)
    {
        if (!mGrabbedRagdoll || mGrabbedRagdollBodyIndex < 0)
            return;

        // Verify ragdoll is still valid
        if (!mGrabbedRagdoll->isValid())
        {
            Log(Debug::Warning) << "Grabbed ragdoll is invalid, releasing";
            mGrabbedRagdoll = nullptr;
            mGrabbedRagdollBodyIndex = -1;
            return;
        }

        mGrabTargetPosition = targetPosition;

        // Get current position of the grabbed body
        osg::Vec3f currentPos = mGrabbedRagdoll->getBodyPosition(mGrabbedRagdollBodyIndex);

        // Calculate displacement to target
        osg::Vec3f displacement = mGrabTargetPosition - currentPos;
        float distance = displacement.length();

        // Movement parameters - slightly softer than dynamic objects for ragdoll feel
        constexpr float moveSpeed = 12.0f;      // How fast the body moves toward target
        constexpr float maxSpeed = 1200.0f;     // Maximum velocity
        constexpr float snapDistance = 1.0f;    // Distance at which we consider arrived

        osg::Vec3f targetVelocity;

        if (distance < snapDistance)
        {
            // Very close - slow down
            targetVelocity = displacement * moveSpeed * 0.5f;
        }
        else
        {
            // Move toward target
            osg::Vec3f direction = displacement / distance;
            float speed = std::min(distance * moveSpeed, maxSpeed);
            targetVelocity = direction * speed;
        }

        // Set velocity on the grabbed body
        mGrabbedRagdoll->setBodyVelocity(mGrabbedRagdollBodyIndex, targetVelocity);

        // Reduce angular velocity to prevent spinning
        mGrabbedRagdoll->setBodyAngularVelocity(mGrabbedRagdollBodyIndex, osg::Vec3f(0, 0, 0));

        // Keep the ragdoll active
        mGrabbedRagdoll->activate();
    }

    MWWorld::Ptr PhysicsSystem::getGrabbedRagdollPtr() const
    {
        if (mGrabbedRagdoll)
            return mGrabbedRagdoll->getPtr();
        return MWWorld::Ptr();
    }
    void PhysicsSystem::createNpcRagdoll(const MWWorld::Ptr& ptr,
        const std::map<std::string, osg::Matrixf, std::less<>>& bones, const osg::Vec3f& kick)
    {
        if (mBrutalRagdolls.contains(ptr.mRef))
            throw std::runtime_error("NPC already has a ragdoll");
        Actor* actor = getActor(ptr);
        if (!actor)
            throw std::runtime_error("NPC has no active physics actor");
        // Publish the new body at a batch boundary, so the previous frame cannot
        // step it before its initial velocity/point impulse has been captured.
        mTaskScheduler->syncSimulation();
        // NPC death hands off to the ragdoll directly, with or without enhanced
        // drives. Keep the actor's translation continuous in both cases.
        const osg::Vec3f inherited = actor->getSimulationVelocity();
        const auto sample = mRagdollAnimation.find(ptr.mRef);
        const auto motion = Settings::game().mRagdollEnhancement && sample != mRagdollAnimation.end()
            ? sample->second.motion : Ragdoll::BoneMotion{};
        const auto* animation = MWBase::Environment::get().getWorld()->getAnimation(ptr);
        Ragdoll::UnsupportedExtremityBones unsupportedBones;
        const auto surfaceVertices = animation ? animation->getRagdollExtremityVertices(&unsupportedBones)
                                              : Ragdoll::ExtremityVertices{};
        const auto* rig = findCreatureRig(ptr);
        auto ragdoll = rig
            ? std::make_unique<Ragdoll>(ptr, *rig, bones, inherited, kick, mTaskScheduler.get())
            : std::make_unique<Ragdoll>(
                ptr, bones, inherited, kick, mTaskScheduler.get(), std::nullopt, motion, surfaceVertices, unsupportedBones);
        mRagdollAnimation.erase(ptr.mRef);
        actor->setRagdoll(true);
        mBrutalRagdolls.emplace(ptr.mRef, std::move(ragdoll));
        MWBase::Environment::get().getWorld()->releaseNpcRagdollEquipment(ptr);
    }

    void PhysicsSystem::stopNpcRagdoll(const MWWorld::Ptr& ptr)
    {
        mSuperKickSources.erase(ptr.mRef);
        mSuperKickVictims.erase(ptr.mRef);
        const auto found = mBrutalRagdolls.find(ptr.mRef);
        if (found == mBrutalRagdolls.end())
            return;
        mPushRagdolls.erase(ptr.mRef);
        mBrutalRagdolls.erase(found); // remove constraints before bodies, while scheduler is alive
        if (auto* actor = getActor(ptr))
        {
            actor->setRagdoll(false);
            actor->updatePosition();
        }
        if (auto* animation = MWBase::Environment::get().getWorld()->getAnimation(ptr))
            animation->endRagdollPose();
    }

    namespace
    {
        // Dark Messiah's kick reactions (ACT_PUSH_*): the side the kick came from, head or
        // body by where the kicker aims, and a random strength among small/medium/long.
        std::string chooseKickReaction(const MWWorld::Ptr& ptr, const MWWorld::Ptr& attacker,
            const osg::Vec3f& direction, float height)
        {
            const float yaw = ptr.getRefData().getPosition().rot[2];
            const osg::Vec3f forward(std::sin(yaw), std::cos(yaw), 0.f), right(std::cos(yaw), -std::sin(yaw), 0.f);
            const osg::Vec3f toAttacker = -direction; // the kick pushes away from its source
            const float front = toAttacker * forward, side = toAttacker * right;
            const char* from = std::abs(front) >= std::abs(side) ? (front >= 0.f ? "front" : "back")
                                                                  : (side >= 0.f ? "right" : "left");
            auto& prng = MWBase::Environment::get().getWorld()->getPrng();
            const int roll = Misc::Rng::rollDice(100, prng);
            if (roll >= 85)
                return std::string("brutal_push_") + from + "_long";
            if (roll >= 60)
                return std::string("brutal_push_") + from + "_medium";
            bool head = false;
            if (!attacker.isEmpty())
            {
                // DM picks the kick height from the kicker's view pitch. The player's eye is at
                // a Morrowind NPC's head height, so a level view kicks the body; looking up
                // over the target's head lands a high kick.
                const auto& source = attacker.getRefData().getPosition();
                const osg::Vec3f target = ptr.getRefData().getPosition().asVec3();
                const float distance = osg::Vec2f(target.x() - source.pos[0], target.y() - source.pos[1]).length();
                const float eye = source.pos[2] + (attacker == MWMechanics::getPlayer() ? 124.7f : height * .9f);
                head = eye - std::tan(source.rot[0]) * distance > target.z() + std::max(height, 128.f) + 16.f;
            }
            return std::string("brutal_push_") + (head ? "head_" : "body_") + from;
        }
    }

    bool PhysicsSystem::applyNpcKick(const MWWorld::Ptr& ptr, const osg::Vec3f& kick, const MWWorld::Ptr& attacker, bool continuing,
        bool knockDown, bool slideTrip)
    {
        return applyNpcKickImpl(ptr, kick, attacker, continuing, knockDown, false, slideTrip);
    }

    bool PhysicsSystem::applyNpcKickImpl(const MWWorld::Ptr& ptr, const osg::Vec3f& kick,
        const MWWorld::Ptr& attacker, bool continuing, bool knockDown, bool superKick, bool slideTrip)
    {
        auto world=MWBase::Environment::get().getWorld();
        auto* actor=getActor(ptr);
        auto* animation=world->getAnimation(ptr);
        const bool isCreature = ptr.getType()==ESM::REC_CREA;
        if ((ptr.getType()!=ESM::REC_NPC_ && !isCreature) || ptr==MWMechanics::getPlayer() || !actor || !animation
            || ptr.getClass().getCreatureStats(ptr).isDead() || animation->isRagdollPose()) return false;
        const auto* rig = findCreatureRig(ptr);
        // A heavy creature answers more slowly; one with no legs, wings or fins cannot be moved at all.
        const auto* record = isCreature ? ptr.get<ESM::Creature>()->mBase : nullptr;
        const bool immobile = record && !(record->mFlags
            & (ESM::Creature::Walks | ESM::Creature::Flies | ESM::Creature::Swims | ESM::Creature::Bipedal));
        // A continuing push has already been scaled when it began.
        const auto* baseNode = ptr.getRefData().getBaseNode();
        const float weight = rig && !continuing ? rig->kickScale(baseNode ? baseNode->getScale().x() : 1.f) : 1.f;
        // The vertical component lifts only a ledge ragdoll: a standing recoil stays grounded.
        const osg::Vec3f push = immobile ? osg::Vec3f() : osg::Vec3f(kick.x(),kick.y(),0.f)*weight;
        osg::Vec3f direction=immobile ? osg::Vec3f(kick.x(),kick.y(),0.f) : push;
        if (direction.normalize()<.01f)
        {
            if (!superKick) return false;
            direction = osg::Vec3f(0, 1, 0); // A spent projectile can still knock a witnessed victim down.
        }
        const auto position=ptr.getRefData().getPosition().asVec3();
        const auto half=actor->getRenderingHalfExtents();
        // Source human hull is 72 units tall. Scale the authored 60/40 probes
        // to this world NPC, independently of the first-person camera/assets.
        const float units=2.f*half.z()/72.f;
        const int mask=Layers::WORLD | Layers::HEIGHTMAP | Layers::DOOR;
        const osg::Vec3f chest=position+osg::Vec3f(0,0,half.z());
        bool fullKnockdown = superKick || (knockDown && !slideTrip);
        bool ledge=Settings::game().mBrutalRagdoll && (fullKnockdown || ((knockDown || actor->getOnGround() || continuing)
            && !world->isSwimming(ptr) && !world->isFlying(ptr)));
        if (isCreature && (!rig || (immobile && !superKick))) ledge=false;
        if (ledge && !knockDown && castSphere(chest,chest+direction*(60.f*units),half.x()*.8f,mask).mHit) ledge=false;
        const osg::Vec3f side(-direction.y(),direction.x(),0);
        // A knockdown falls on open ground; only a kick needs the void behind the target.
        if (ledge && !knockDown)
            for (float lateral : {-0.7f,0.f,0.7f})
            {
                const auto p=position+direction*(60.f*units)+side*(half.x()*lateral);
                const auto support=castRay(p+osg::Vec3f(0,0,8),p-osg::Vec3f(0,0,40.f*units),mask);
                if (support.mHit && support.mHitNormal.z()>.5f) { ledge=false; break; }
            }
        Ragdoll::BoneMatrices bones;
        if (ledge && rig)
        {
            for (const auto& part:rig->mParts)
            { auto bone=animation->getBoneWorldMatrix(part.mBone); if (!bone) {ledge=false;break;} bones.emplace(part.mBone,*bone); }
        }
        else if (ledge) for (const auto& name:Ragdoll::getRequiredBones())
        { auto bone=animation->getBoneWorldMatrix(name); if (!bone) {ledge=false;break;} bones.emplace(name,*bone); }
        if (!ledge)
        {
            if (superKick) return false; // Missing bones must not turn a rejected power kick into an ordinary push.
            if (!continuing)
            {
                if (!immobile) actor->addExternalVelocity(push);
                // Creatures keep their own hit animations: no group is imposed on them.
                MWBase::Environment::get().getMechanicsManager()->staggerActor(
                    ptr, isCreature ? std::string() : chooseKickReaction(ptr, attacker, direction, 2.f * half.z()));
                // Source rechecks support throughout a push, not only at contact.
                if (!isCreature || (rig && !immobile))
                    mKickWatches[ptr.mRef]={ptr,attacker,direction,0.f,kick.z()*weight/push.length()};
            }
            return false;
        }
        // A fresh kick has not entered external velocity. A continuing push
        // already has: transfer its remaining component into a torso impulse,
        // subtracting it from inherited COM motion so momentum is not doubled.
        mTaskScheduler->syncSimulation();
        bool clear=false;
        const auto pending=actor->consumeExternalVelocity(clear);
        const auto inherited=(clear ? osg::Vec3f() : actor->getSimulationVelocity())+pending-(continuing ? push : osg::Vec3f());
        const bool lowTrip = slideTrip && knockDown && !superKick && !continuing;
        // The sliding boot removes support at the posed shins. Ordinary kicks
        // and continuing ledge pushes still enter through the chest.
        const osg::Vec3f rootBone = rig ? osg::Vec3f(bones.at(rig->mParts.front().mBone).getTrans()) : osg::Vec3f();
        const osg::Vec3f humanContact = rig ? osg::Vec3f() : osg::Vec3f(lowTrip
            ? (bones.at("Bip01 L Calf").getTrans() + bones.at("Bip01 L Foot").getTrans()
                  + bones.at("Bip01 R Calf").getTrans() + bones.at("Bip01 R Foot").getTrans()) * .25f
                - direction * 5.f // physical calf capsule radius
            : bones.at("Bip01 Spine1").getTrans() - direction * half.x());
        const osg::Vec3f contact = rig
            ? osg::Vec3f((lowTrip ? osg::Vec3f(position + osg::Vec3f(0.f, 0.f, half.z() * .3f)) : rootBone) - direction * half.x())
            : humanContact;
        MWBase::Environment::get().getMechanicsManager()->staggerActor(ptr);
        animation->beginRagdollPose();
        try {
            const auto sample = mRagdollAnimation.find(ptr.mRef);
            const auto motion = Settings::game().mRagdollEnhancement && sample != mRagdollAnimation.end()
                ? sample->second.motion : Ragdoll::BoneMotion{};
            Ragdoll::UnsupportedExtremityBones unsupportedBones;
            const auto surfaceVertices = animation->getRagdollExtremityVertices(&unsupportedBones);
            const auto profile = lowTrip ? Ragdoll::KickProfile::LowTrip : Ragdoll::KickProfile::Chest;
            auto body = rig
                ? std::make_unique<Ragdoll>(ptr,*rig,bones,inherited,kick*weight,mTaskScheduler.get(),contact,profile)
                : std::make_unique<Ragdoll>(ptr,bones,inherited,kick,mTaskScheduler.get(),contact,motion,
                    surfaceVertices,unsupportedBones,profile);
            if (fullKnockdown) body->guardSuperKickSpawn();
            mRagdollAnimation.erase(ptr.mRef);
            actor->clearExternalVelocity(); actor->setInertialForce({}); actor->setOnGround(false); actor->setRagdoll(true);
            mBrutalRagdolls.emplace(ptr.mRef,std::move(body));
            mKickWatches.erase(ptr.mRef);
            auto& state=mPushRagdolls[ptr.mRef]; state.peakZ=state.previousZ=position.z(); state.attacker=attacker;
            state.knockDown=knockDown;
            // Every living ragdoll is a disarm, including ordinary knockdowns.
            world->releaseNpcRagdollEquipment(ptr);
        } catch (...) {animation->endRagdollPose();throw;}
        if (!attacker.isEmpty() && !knockDown)
            MWBase::Environment::get().getLuaManager()->npcKickRagdoll(attacker, ptr);
        return true;
    }

    bool PhysicsSystem::applyNpcSuperKick(const MWWorld::Ptr& ptr, const osg::Vec3f& velocity,
        const MWWorld::Ptr& attacker, std::uint64_t kickId)
    {
        if (!Settings::game().mBrutalRagdoll || ptr.isEmpty() || (ptr.getType() != ESM::REC_NPC_ && !findCreatureRig(ptr)) || ptr == MWMechanics::getPlayer()
            || !kickId || kickId > 9007199254740991ULL || mSuperKickIds.contains(kickId)
            || mSuperKickSources.contains(ptr.mRef)) return false;
        for (int axis = 0; axis < 3; ++axis) if (!std::isfinite(velocity[axis])) return false;
        if (velocity.length2() > 1000.f * 1000.f
            || osg::Vec2f(velocity.x(), velocity.y()).length() < 100.f) return false;
        auto* actor = getActor(ptr);
        auto world = MWBase::Environment::get().getWorld();
        auto* animation = world->getAnimation(ptr);
        if (!actor || !actor->isActive() || !animation || !ptr.getRefData().isEnabled()) return false;
        mTaskScheduler->syncSimulation();
        if (!mBrutalRagdolls.contains(ptr.mRef))
        {
            if (!ptr.getClass().getCreatureStats(ptr).isDead())
            {
                if (!applyNpcKickImpl(ptr, velocity, attacker, false, true, true)) return false;
            }
            else
            {
                // Foot damage can kill immediately before the queued super-kick
                // action. Hand that current pose to physics without waiting for
                // an ordinary death-animation frame, then launch the same body.
                Ragdoll::BoneMatrices bones;
                std::vector<std::string> names;
                if (const auto* rig = findCreatureRig(ptr))
                    for (const auto& part : rig->mParts) names.push_back(part.mBone);
                else names = Ragdoll::getRequiredBones();
                for (const auto& name : names)
                {
                    const auto bone = animation->getBoneWorldMatrix(name);
                    if (!bone) return false;
                    bones.emplace(name, *bone);
                }
                animation->beginRagdollPose();
                try { createNpcRagdoll(ptr, bones, osg::Vec3f()); }
                catch (...) { animation->endRagdollPose(); throw; }
                mBrutalRagdolls.at(ptr.mRef)->guardSuperKickSpawn();
            }
        }
        auto& body = *mBrutalRagdolls.at(ptr.mRef);
        const auto* source = attacker.isEmpty() ? nullptr : getActor(attacker);
        if (!body.startSuperKick(kickId, kickId, velocity, source ? source->getPhysicsBody() : JPH::BodyID())) return false;
        mSuperKickIds.insert(kickId);
        mSuperKickSources.emplace(ptr.mRef, SuperKickSource{ attacker, kickId });
        mSuperKickVictims.erase(ptr.mRef);
        if (auto push = mPushRagdolls.find(ptr.mRef); push != mPushRagdolls.end())
        {
            push->second = PushRagdoll{};
            push->second.attacker = attacker;
            push->second.peakZ = push->second.previousZ = body.getDynamics()[0].z();
            push->second.knockDown = true;
        }
        world->releaseNpcRagdollEquipment(ptr);
        return true;
    }

    std::map<std::string, double> PhysicsSystem::getNpcSuperKickState(const MWWorld::Ptr& ptr) const
    {
        const auto body = mBrutalRagdolls.find(ptr.mRef);
        if (body == mBrutalRagdolls.end()) return {};
        auto result = body->second->getSuperKickState();
        if (const auto source = mSuperKickSources.find(ptr.mRef); source != mSuperKickSources.end())
        {
            result["secondary"] = 0;
            result["knockdowns"] = source->second.knockdowns;
            result["rejectedContacts"] = source->second.rejectedContacts;
        }
        else if (const auto victim = mSuperKickVictims.find(ptr.mRef); victim != mSuperKickVictims.end())
        {
            result["active"] = 0;
            result["secondary"] = 1;
            result["kickId"] = double(victim->second);
            result["receivedHits"] = 1;
        }
        return result;
    }

    void PhysicsSystem::processSuperKickContacts()
    {
        if (mSuperKickSources.empty()) return;
        std::vector<Ragdoll::SuperKickContact> contacts;
        for (const auto& [ref, body] : mBrutalRagdolls)
        {
            auto pending = body->takeSuperKickContacts();
            contacts.insert(contacts.end(), pending.begin(), pending.end());
        }
        for (const auto& contact : contacts)
        {
            const auto source = std::find_if(mSuperKickSources.begin(), mSuperKickSources.end(),
                [&](const auto& item) { return item.second.kickId == contact.token; });
            if (source == mSuperKickSources.end()) continue;
            const auto attacker = source->second.attacker;
            MWWorld::Ptr target;
            for (const auto& [ref, actor] : mActors)
                if (actor->getPhysicsBody() == contact.actor)
                { target = actor->getPtr(); break; }
            bool applied = false;
            if (!target.isEmpty() && target.getClass().isActor() && target != MWMechanics::getPlayer()
                && target.getRefData().isEnabled() && !target.getClass().getCreatureStats(target).isDead())
                if (const auto* actor = getActor(target); actor && actor->isActive() && !actor->isRagdoll())
                    applied = applyNpcKickImpl(target, contact.transfer, attacker, false, true, true);
            // World equipment release may move references. Resolve the source
            // again instead of holding iterators across a main-thread World call.
            const auto current = std::find_if(mSuperKickSources.begin(), mSuperKickSources.end(),
                [&](const auto& item) { return item.second.kickId == contact.token; });
            if (current != mSuperKickSources.end())
            {
                if (applied) ++current->second.knockdowns;
                else ++current->second.rejectedContacts;
            }
            if (applied) mSuperKickVictims[target.mRef] = contact.token;
        }
    }

    bool PhysicsSystem::detachNpcRagdollHead(const MWWorld::Ptr& ptr, const osg::Vec3f& cutVelocity,
        const osg::Vec3f& headAngularImpulse)
    {
        if (ptr == MWMechanics::getPlayer() || !ptr.getClass().isNpc()
            || !ptr.getClass().getCreatureStats(ptr).isDead())
            return false;
        const auto found = mBrutalRagdolls.find(ptr.mRef);
        return found != mBrutalRagdolls.end() && found->second->detachHead(cutVelocity, headAngularImpulse);
    }

    bool PhysicsSystem::detachNpcRagdollTorso(const MWWorld::Ptr& ptr, const osg::Vec3f& cutVelocity,
        const osg::Vec3f& upperSeparation, const std::string& savedPose)
    {
        if (ptr == MWMechanics::getPlayer() || !ptr.getClass().isNpc()
            || !ptr.getClass().getCreatureStats(ptr).isDead()) return false;
        const auto found = mBrutalRagdolls.find(ptr.mRef);
        if (found == mBrutalRagdolls.end() || !found->second->detachTorso(cutVelocity, upperSeparation, savedPose)) return false;
        if (!savedPose.empty())
            if (auto* animation = MWBase::Environment::get().getWorld()->getAnimation(ptr))
                for (const auto& [bone, matrix] : found->second->getBonePoses(1.f))
                    animation->setBoneWorldMatrix(bone, matrix);
        return true;
    }

    std::string PhysicsSystem::getNpcBisectionPoseData(const MWWorld::Ptr& ptr) const
    {
        const auto found = mBrutalRagdolls.find(ptr.mRef);
        return found == mBrutalRagdolls.end() ? std::string() : found->second->getBisectionPoseData();
    }

    bool PhysicsSystem::detachNpcRagdollLimbs(const MWWorld::Ptr& ptr, unsigned mask,
        const osg::Vec3f& cutVelocity, const std::string& savedPose)
    {
        if (ptr == MWMechanics::getPlayer() || !ptr.getClass().isNpc()
            || !ptr.getClass().getCreatureStats(ptr).isDead()) return false;
        const auto found = mBrutalRagdolls.find(ptr.mRef);
        if (found == mBrutalRagdolls.end() || !found->second->detachLimbs(mask, cutVelocity, savedPose)) return false;
        if (!savedPose.empty())
            if (auto* animation = MWBase::Environment::get().getWorld()->getAnimation(ptr))
                for (const auto& [bone, matrix] : found->second->getBonePoses(1.f))
                    animation->setBoneWorldMatrix(bone, matrix);
        return true;
    }

    std::string PhysicsSystem::getNpcLimbSeveringPoseData(const MWWorld::Ptr& ptr) const
    {
        const auto found = mBrutalRagdolls.find(ptr.mRef);
        return found == mBrutalRagdolls.end() ? std::string() : found->second->getLimbSeveringPoseData();
    }

    unsigned PhysicsSystem::getNpcSeveredLimbsMask(const MWWorld::Ptr& ptr) const
    {
        const auto found = mBrutalRagdolls.find(ptr.mRef);
        return found == mBrutalRagdolls.end() ? 0 : found->second->getSeveredLimbsMask();
    }

    MWWorld::Ptr PhysicsSystem::kickNpcRagdoll(const osg::Vec3f& origin, const osg::Vec3f& forward, float reach,
        float minimumDot, const osg::Vec3f& velocity, bool& headHit, osg::Vec3f& hitPoint, std::size_t& hitPart)
    {
        headHit = false;
        Ragdoll* best = nullptr;
        std::size_t bestPart = 0;
        float bestDistance = -1.f;
        for (const auto& [ref, ragdoll] : mBrutalRagdolls)
        {
            std::size_t part = 0;
            osg::Vec3f point;
            const float distance = ragdoll->findKickPart(origin, forward, reach, minimumDot, part, point);
            if (distance >= 0.f && (!best || distance < bestDistance))
            {
                if (castRay(origin, point, Layers::WORLD | Layers::HEIGHTMAP | Layers::DOOR).mHit) continue;
                best = ragdoll.get();
                bestPart = part;
                bestDistance = distance;
                hitPoint = point;
            }
        }
        if (!best)
            return {};
        headHit = best->isDetachedHeadPart(bestPart);
        if (!best->applyKick(bestPart, velocity, hitPoint)) return {};
        hitPart = bestPart;
        return best->getPtr();
    }

    std::vector<osg::Vec3f> PhysicsSystem::getNpcRagdollDynamics(const MWWorld::Ptr& ptr) const
    {
        auto it=mBrutalRagdolls.find(ptr.mRef); if (it==mBrutalRagdolls.end()) return {};
        auto out=it->second->getDynamics();
        auto push=mPushRagdolls.find(ptr.mRef);
        out.emplace_back(push!=mPushRagdolls.end(),push==mPushRagdolls.end()?0.f:push->second.age,it->second->initialRotationError());
        out.emplace_back(it->second->initialMatrixError(),0,0);
        out.push_back(it->second->getLandingState());
        out.emplace_back(push==mPushRagdolls.end()?0.f:push->second.settled,
            push==mPushRagdolls.end()?0.f:push->second.unsupported,
            push!=mPushRagdolls.end() && push->second.supported);
        const auto head = it->second->getHeadState();
        out.insert(out.end(), head.begin(), head.end());
        return out;
    }

    std::map<std::string, double> PhysicsSystem::getNpcRagdollEnhancementState(const MWWorld::Ptr& ptr) const
    {
        const auto found = mBrutalRagdolls.find(ptr.mRef);
        return found == mBrutalRagdolls.end() ? std::map<std::string, double>() : found->second->getEnhancementState();
    }

    bool PhysicsSystem::applyNpcRagdollImpact(const MWWorld::Ptr& ptr, std::size_t part,
        const osg::Vec3f& point, const osg::Vec3f& impulse, std::uint64_t id)
    {
        const auto found = mBrutalRagdolls.find(ptr.mRef);
        return found != mBrutalRagdolls.end() && found->second->applyImpact(part, point, impulse, id);
    }

    void PhysicsSystem::sampleNpcRagdollAnimation(float duration)
    {
        if (!Settings::game().mRagdollEnhancement)
        { mRagdollAnimation.clear(); return; }
        if (!std::isfinite(duration) || duration <= 0.f)
        { mRagdollAnimation.clear(); return; }
        auto world = MWBase::Environment::get().getWorld();
        for (const auto& [ref, actor] : mActors)
        {
            const auto ptr = actor->getPtr();
            if (ptr.getType() != ESM::REC_NPC_ || ptr == MWMechanics::getPlayer()
                || actor->isRagdoll() || !actor->isActive())
            { mRagdollAnimation.erase(ref); continue; }
            if (ptr.getClass().getCreatureStats(ptr).isDead())
            {
                // Scripted death observation can queue activation on a later
                // frame. Retain the last live velocity briefly, without sampling
                // a canned death animation or reviving stale loaded-corpse motion.
                if (const auto found = mRagdollAnimation.find(ref); found != mRagdollAnimation.end())
                {
                    found->second.deadAge += duration;
                    if (found->second.deadAge > .25f) mRagdollAnimation.erase(found);
                }
                continue;
            }
            auto* animation = world->getAnimation(ptr);
            if (!animation) { mRagdollAnimation.erase(ref); continue; }
            Ragdoll::BoneMatrices bones;
            for (const auto& name : Ragdoll::getRequiredBones())
            {
                const auto bone = animation->getBoneWorldMatrix(name);
                if (!bone) { bones.clear(); break; }
                bones.emplace(name, *bone);
            }
            if (bones.empty()) { mRagdollAnimation.erase(ref); continue; }
            // Read the scene root through the same live world-path convention
            // as getBoneWorldMatrix, rather than mixing record/physics coordinates
            // with the evaluated rendered bones.
            const auto* root = ptr.getRefData().getBaseNode();
            if (!root) { mRagdollAnimation.erase(ref); continue; }
            const auto paths = root->getParentalNodePaths();
            if (paths.empty()) { mRagdollAnimation.erase(ref); continue; }
            const osg::Matrixf rootWorld = osg::computeLocalToWorld(paths.front());
            bool finiteRoot = true;
            for (int i = 0; i < 16; ++i)
                finiteRoot = finiteRoot && std::isfinite(rootWorld.ptr()[i]);
            if (!finiteRoot) { mRagdollAnimation.erase(ref); continue; }
            const auto renderedRootPosition = rootWorld.getTrans();
            auto& sample = mRagdollAnimation[ref];
            if (sample.deadAge > 0.f) sample.bones.clear(); // resurrection starts a fresh sample pair
            sample.deadAge = 0.f;
            sample.motion = sample.bones.empty() ? Ragdoll::BoneMotion{}
                : Ragdoll::sampleAnimationMotion(sample.bones, bones,
                    renderedRootPosition - sample.renderedRootPosition, duration);
            sample.bones = std::move(bones);
            sample.renderedRootPosition = renderedRootPosition;
        }
    }

    void PhysicsSystem::moveNpcRagdolls(float duration)
    {
        if (!Settings::game().mBrutalRagdoll)
        {
            if (!mBrutalRagdolls.empty()) mTaskScheduler->syncSimulation();
            std::vector<MWWorld::Ptr> actors;
            for (const auto& [_, body] : mBrutalRagdolls) actors.push_back(body->getPtr());
            for (const auto& actor : actors)
            {
                stopNpcRagdoll(actor);
                MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(actor);
            }
            mKickWatches.clear();
            return;
        }

        auto world = MWBase::Environment::get().getWorld();
        if (!Settings::game().mBrutalDismemberment)
        {
            std::vector<MWWorld::Ptr> cuts;
            for (const auto& [_, body] : mBrutalRagdolls)
                if (body->isTorsoDetached() || body->getSeveredLimbsMask()
                    || (!body->getHeadState().empty() && body->getHeadState().front().x() != 0.f))
                    cuts.push_back(body->getPtr());
            if (!cuts.empty()) mTaskScheduler->syncSimulation();
            for (const auto& actor : cuts)
            {
                stopNpcRagdoll(actor);
                MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(actor);
            }
        }
        std::vector<KickWatch> watches;
        for (auto it=mKickWatches.begin();it!=mKickWatches.end();)
        {
            auto& watch=it->second; watch.age+=duration;
            auto* actor=getActor(watch.actor);
            if (!actor || watch.age>1.f || actor->isExternalVelocityResetPending()
                || watch.actor.getClass().getCreatureStats(watch.actor).isDead()
                || actor->getExternalVelocity()*watch.direction<20.f)
                it=mKickWatches.erase(it);
            else { watches.push_back(watch); ++it; }
        }
        for (const auto& watch:watches)
            if (auto* actor=getActor(watch.actor))
            {
                auto remaining=watch.direction*std::max(0.f,actor->getExternalVelocity()*watch.direction);
                remaining.z()=remaining.length()*watch.lift;
                applyNpcKick(watch.actor,remaining,watch.attacker,true);
            }
        // Otherwise the same frame may select either the old completed pose
        // (alpha=1) or a newly completed batch (residual alpha), depending on
        // worker timing. Include bodies just created by a ledge watch, wait once
        // for the whole group, then select one deterministic residual clock.
        if (!mBrutalRagdolls.empty()) mTaskScheduler->syncSimulation();
        processSuperKickContacts();
        struct Update { MWWorld::Ptr ptr; osg::Vec3f position;
            std::vector<std::pair<std::string, osg::Matrixf>> poses; };
        std::vector<Update> updates;
        std::vector<MWWorld::Ptr> cancel;
        std::vector<MWWorld::Ptr> recover;
        for (const auto& [_, ragdoll] : mBrutalRagdolls)
        {
            ragdoll->setWater(mWaterEnabled, mWaterHeight);
            ragdoll->configureEnhancement();
            const auto ptr = ragdoll->getPtr();
            auto* animation = world->getAnimation(ptr);
            if ((!ptr.getClass().getCreatureStats(ptr).isDead() && !mPushRagdolls.contains(ptr.mRef)) || !animation || !animation->isRagdollPose())
            {
                cancel.push_back(ptr);
                continue;
            }
            auto poses = ragdoll->getBonePoses();
            auto position = ragdoll->getActorPosition(ptr.getRefData().getPosition().asVec3());
            if (position && ptr.getClass().getCreatureStats(ptr).isDead())
            {
                // A corpse's standing pelvis offset places its reference below
                // the floor. That offset cannot be reused with the ordinary
                // completed death pose reconstructed by a saved-game load.
                // Reanchor only the scene reference; the same world bone poses
                // are reapplied below, leaving every rendered limb unchanged.
                position->z() = ragdoll->retainedBottom();
            }
            if (auto it=mPushRagdolls.find(ptr.mRef); it!=mPushRagdolls.end())
            {
                auto& push=it->second; push.age+=duration;
                auto& stats=ptr.getClass().getCreatureStats(ptr);
                const auto dynamics=ragdoll->getDynamics();
                const float z=dynamics[0].z();
                const int mask=Layers::WORLD | Layers::HEIGHTMAP | Layers::DOOR;
                const auto floor=castRay(dynamics[0],osg::Vec3f(dynamics[0].x(),dynamics[0].y(),ragdoll->bottom()-12.f),mask);
                const bool supported=floor.mHit && floor.mHitNormal.z()>.5f && ragdoll->bottom()-floor.mHitPos.z()<12.f;
                push.supported=supported;
                if (supported && push.landed)
                {
                    // A later departure starts at its actual last supported
                    // height, not the peak of an earlier completed fall.
                    push.peakZ=z; push.downSpeed=0.f;
                }
                else
                {
                    push.peakZ=std::max(push.peakZ,z);
                    push.downSpeed=std::max(push.downSpeed,-dynamics[1].z());
                }
                push.unsupported=supported?0.f:push.unsupported+duration;
                // Rearm only after real unsupported flight. Rest contact jitter
                // cannot consume or repeatedly charge the same landing.
                if (push.unsupported>.05f) push.landed=false;
                if (!push.landed && push.downSpeed>80.f && dynamics[1].z()>-30.f && supported)
                {
                    push.landed=true;
                    const float height=std::max(0.f,push.peakZ-z);
                    const float damage=MWMechanics::calculateRagdollFallDamage(ptr,height);
                    ragdoll->recordLanding(height,damage);
                    if (damage>0.f && !stats.isDead()) ptr.getClass().onHit(ptr,{{"health",damage}},ESM::RefId(),push.attacker,true,MWMechanics::DamageSourceType::Unspecified);
                }
                if (position) { position->x()=dynamics[0].x();position->y()=dynamics[0].y();position->z()=ragdoll->bottom(); }
                if (supported && ragdoll->speed()<35.f && ragdoll->angularSpeed()<1.5f) push.settled+=duration;
                else push.settled=0.f;
                if (stats.isDead()) mPushRagdolls.erase(it); // retain the same body/momentum as a corpse
                // In water there is nothing to come to rest on: the actor swims again.
                else if (push.age>=2.f && (push.settled>=.35f || (mWaterEnabled && z<mWaterHeight))) recover.push_back(ptr);
            }
            if (position)
                updates.push_back({ptr, *position, std::move(poses)});
        }
        for (const auto& ptr : cancel)
            stopNpcRagdoll(ptr);
        for (const auto& update : updates)
        {
            // Cell moves can replace references or unload a corpse; never keep map iterators across World calls.
            const auto ptr = world->moveObject(update.ptr, update.position, false, false);
            if (!mBrutalRagdolls.contains(ptr.mRef))
                continue;
            if (auto* animation = world->getAnimation(ptr); animation && animation->isRagdollPose())
                for (const auto& [bone, matrix] : update.poses)
                    animation->setBoneWorldMatrix(bone, matrix);
        }
        for (const auto& ptr:recover)
        {
            auto* animation=world->getAnimation(ptr);
            std::string_view standUp;
            if (animation && Settings::game().mDarkMessiahStandUp && !Settings::game().mBrutalClassicNpcAnimations)
                if (const auto found=mBrutalRagdolls.find(ptr.mRef); found!=mBrutalRagdolls.end())
                {
                    // SCHED_MM_STAND_UP_FRONT/BACK. Both clips start lying with the head behind the
                    // actor's final facing, face down (front) or face up (back). Turn the actor to
                    // face the ragdoll's feet, then reapply the same world bone poses so the short
                    // recovery blend starts from exactly what is on screen.
                    // A copy: rotating the actor resamples the ragdoll's pose list.
                    const std::vector<std::pair<std::string, osg::Matrixf>> poses=found->second->getLastBonePoses();
                    const auto at=[&](std::string_view name) -> std::optional<osg::Vec3f> {
                        for (const auto& [bone,matrix]:poses) if (bone==name) return matrix.getTrans();
                        return std::nullopt; };
                    const auto pelvis=at("Bip01 Pelvis"), head=at("Bip01 Head");
                    const auto left=at("Bip01 L UpperArm"), right=at("Bip01 R UpperArm");
                    if (pelvis && head && left && right)
                    {
                        const osg::Vec3f up=*head-*pelvis;
                        osg::Vec3f feet(-up.x(),-up.y(),0.f);
                        const osg::Vec3f front=(*left-*right)^up;
                        const std::string_view group=front.z()<0.f ? "brutal_standup_front" : "brutal_standup_back";
                        if (feet.normalize()>1e-3f && animation->hasAnimation(group))
                        {
                            world->rotateObject(ptr,osg::Vec3f(0.f,0.f,std::atan2(feet.x(),feet.y())));
                            for (const auto& [bone,matrix]:poses) animation->setBoneWorldMatrix(bone,matrix);
                            standUp=group;
                        }
                    }
                }
            if (animation) animation->endRagdollPose(standUp.empty() ? .6f : .25f);
            stopNpcRagdoll(ptr);
            if (auto* actor=getActor(ptr)) {actor->setOnGround(false);actor->setInertialForce({});}
            MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(ptr);
            if (!standUp.empty()) MWBase::Environment::get().getMechanicsManager()->staggerActor(ptr,standUp);
        }
    }

    std::vector<osg::Vec3f> PhysicsSystem::getNpcRagdollState(const MWWorld::Ptr& ptr) const
    {
        const auto found = mBrutalRagdolls.find(ptr.mRef);
        if (found == mBrutalRagdolls.end())
            return {};
        const auto* animation = MWBase::Environment::get().getWorld()->getAnimation(ptr);
        if (!animation)
            return {};
        const auto& poses = found->second->getLastBonePoses();
        std::vector<osg::Vec3f> state;
        state.emplace_back(static_cast<float>(found->second->partCount()), found->second->maxJointError(),
            found->second->retainedBottom());
        for (const auto& [bone, matrix] : poses)
        {
            state.push_back(matrix.getTrans());
            const auto rendered = animation->getBoneWorldMatrix(bone);
            state.push_back(rendered ? osg::Vec3f(rendered->getTrans()) : osg::Vec3f());
        }
        return state;
    }

}
