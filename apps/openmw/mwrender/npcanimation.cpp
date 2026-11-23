#include "../mwphysics/collisiontype.hpp"
#include "npcanimation.hpp"
#include "npcblood.hpp"
#include "decapitation.hpp"
#include "bisection.hpp"
#include "limbsevering.hpp"
#include "releasedequipmentcapture.hpp"
#include "decapitationplane.hpp"
#include "renderprewarm.hpp"
#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/bonepieces.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <osg/Texture2D>
#include <cmath>
#include <limits>
#include <chrono>
#include <mutex>
#include <set>
#include <tuple>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>
#include <osg/observer_ptr>
#include <osg/FrameStamp>
#include <osg/NodeCallback>
#include <osg/Billboard>
#include <osgUtil/IncrementalCompileOperation>

#include <osg/Depth>
#include <osg/MatrixTransform>
#include <osg/UserDataContainer>

#include <osgUtil/CullVisitor>
#include <osgUtil/RenderBin>

#include <components/debug/debuglog.hpp>

#include <components/misc/rng.hpp>
#include <components/misc/strings/algorithm.hpp>

#include <components/misc/resourcehelpers.hpp>

#include <components/esm3/loadbody.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/keyframe.hpp>
#include <components/sceneutil/bodypartcoverage.hpp>
#include <components/sceneutil/hemcoverage.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/settings/values.hpp>

#include <components/vfs/manager.hpp>

#include "../mwmechanics/brutaloptions.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/inventorystore.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/npcstats.hpp"
#include "../mwmechanics/weapontype.hpp"
#include "../mwphysics/raycasting.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/globalvariablename.hpp"

#include "actorutil.hpp"
#include "jiggleautorig.hpp"
#include "postprocessor.hpp"
#include "renderbin.hpp"
#include "renderingmanager.hpp"
#include "camera.hpp"
#include "rotatecontroller.hpp"
#include "vismask.hpp"

namespace
{

    bool decapitationAuditEnabled()
    {
        const auto* directory = std::getenv("OPENMW_DECAPITATION_AUDIT_DIR");
        return directory && *directory;
    }

    // Explicitly enabled private lab output only. Never dump model geometry to a
    // default user directory or include it in the public project artifacts.
    void dumpRejectedDecapitation(const osg::Matrixf& neck, const osg::Matrixf& head,
        const std::vector<osg::Vec3f>& retainedPoints, const std::vector<osg::Vec3f>& neckPoints,
        const std::vector<osg::Vec3f>& headPoints,
        const std::vector<std::array<osg::Vec3f, 3>>& neckTriangles,
        const std::vector<MWRender::DecapitationGeometry::Snapshot>& snapshots,
        const std::vector<ESM::PartReferenceType>& parts,
        const std::vector<osg::ref_ptr<osg::Geometry>>& retainedSurfaces,
        const MWRender::AnatomicalCutSearchDiagnostics& diagnostics)
    {
        const auto* directory = std::getenv("OPENMW_DECAPITATION_AUDIT_DIR");
        if (!directory || !*directory)
            return;
        try
        {
            const std::filesystem::path folder(directory);
            if (!std::filesystem::is_directory(folder))
            {
                Log(Debug::Warning) << "Decapitation audit directory does not exist: " << folder;
                return;
            }
            static std::atomic<unsigned> sequence{ 0 };
            const auto time = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto path = folder / ("decap-rejected-" + std::to_string(time)
                + "-" + std::to_string(++sequence) + ".json");
            std::ofstream out(path);
            if (!out)
                throw std::runtime_error("could not open private output");
            out.imbue(std::locale::classic());
            out << std::setprecision(std::numeric_limits<float>::max_digits10);
            const auto number = [&](float value) {
                if (std::isfinite(value)) out << value;
                else out << "null";
            };
            const auto vector = [&](const osg::Vec3f& value) {
                out << '['; number(value.x()); out << ','; number(value.y()); out << ','; number(value.z()); out << ']';
            };
            const auto points = [&](const std::vector<osg::Vec3f>& values) {
                out << '[';
                for (std::size_t i = 0; i < values.size(); ++i)
                { if (i) out << ','; vector(values[i]); }
                out << ']';
            };
            const auto matrix = [&](const osg::Matrixf& value) {
                out << '[';
                for (int i = 0; i < 16; ++i) { if (i) out << ','; number(value.ptr()[i]); }
                out << ']';
            };
            const auto geometry = [&](const osg::Geometry& value) {
                out << "\"vertices\":[";
                const auto* vertices = dynamic_cast<const osg::Vec3Array*>(value.getVertexArray());
                if (vertices)
                    for (std::size_t i = 0; i < vertices->size(); ++i)
                    { if (i) out << ','; vector((*vertices)[i]); }
                osg::TriangleIndexFunctor<MWRender::DecapitationGeometry::TriangleList> triangles;
                value.accept(triangles);
                out << "],\"triangles\":[";
                for (std::size_t i = 0; i < triangles.triangles.size(); ++i)
                {
                    if (i) out << ',';
                    const auto& t = triangles.triangles[i];
                    out << '[' << t[0] << ',' << t[1] << ',' << t[2] << ']';
                }
                out << ']';
            };
            out << "{\"version\":1,\"space\":\"world native units; OSG row-vector matrices in ptr order\",\n";
            out << "\"neckOrigin\":"; vector(neck.getTrans());
            out << ",\"headOrigin\":"; vector(head.getTrans());
            out << ",\"headUp\":"; vector(osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), head));
            out << ",\"neckUp\":"; vector(osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), neck));
            out << ",\"headSide\":"; vector(osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), head));
            out << ",\"neckMatrix\":"; matrix(neck);
            out << ",\"headMatrix\":"; matrix(head);
            out << ",\n\"retainedPoints\":"; points(retainedPoints);
            out << ",\n\"neckPoints\":"; points(neckPoints);
            out << ",\n\"headPoints\":"; points(headPoints);
            out << ",\n\"neckTriangles\":[";
            for (std::size_t i = 0; i < neckTriangles.size(); ++i)
            {
                if (i) out << ',';
                out << '['; vector(neckTriangles[i][0]); out << ',';
                vector(neckTriangles[i][1]); out << ','; vector(neckTriangles[i][2]); out << ']';
            }
            out << "],\n\"guards\":{\"retainedMargin\":0.15,\"neckMargin\":0.05,"
                "\"headMargin\":0.05,\"boneMargin\":0.05,\"minimumClearance\":0.1,"
                "\"minimumAxisDot\":0.25,\"contourWeldDistance\":0.01,\"minimumContourArea\":0.01,"
                "\"maximumContourRadius\":40},\n\"search\":{\"invalidInput\":" << diagnostics.invalidInput
                << ",\"normalCandidates\":" << diagnostics.normalCandidates
                << ",\"axisRejected\":" << diagnostics.axisRejected
                << ",\"intervalRejected\":" << diagnostics.intervalRejected
                << ",\"scoreSkipped\":" << diagnostics.scoreSkipped
                << ",\"contourTests\":" << diagnostics.contourTests
                << ",\"contourRejected\":" << diagnostics.contourRejected
                << ",\"acceptedCandidates\":" << diagnostics.acceptedCandidates << ",\"bestClearance\":";
            number(diagnostics.bestClearance);
            out << ",\"bestLower\":"; number(diagnostics.bestLower);
            out << ",\"bestUpper\":"; number(diagnostics.bestUpper);
            out << ",\"bestNormal\":"; vector(diagnostics.bestNormal);
            out << ",\"fallbackNormalCandidates\":" << diagnostics.fallbackNormalCandidates
                << ",\"fallbackContourTests\":" << diagnostics.fallbackContourTests
                << ",\"fallbackSlabTests\":" << diagnostics.fallbackSlabTests
                << ",\"fallbackBudgetExhausted\":" << diagnostics.fallbackBudgetExhausted;
            out << "},\n\"headPart\":" << int(ESM::PRT_Head) << ",\"neckPart\":" << int(ESM::PRT_Neck)
                << ",\"snapshots\":[";
            for (std::size_t i = 0; i < snapshots.size(); ++i)
            {
                if (i) out << ',';
                out << "{\"part\":" << int(parts[i]) << ",\"hair\":" << snapshots[i].hair
                    << ",\"helmet\":" << snapshots[i].helmet << ',';
                geometry(*snapshots[i].geometry);
                out << '}';
            }
            out << "],\n\"retainedSurfaces\":[";
            for (std::size_t i = 0; i < retainedSurfaces.size(); ++i)
            {
                if (i) out << ',';
                out << '{'; geometry(*retainedSurfaces[i]); out << '}';
            }
            out << "]}\n";
            out.flush();
            if (!out)
                throw std::runtime_error("could not finish private output");
            Log(Debug::Warning) << "Decapitation private rejection audit: " << path;
        }
        catch (const std::exception& error)
        { Log(Debug::Warning) << "Decapitation audit output failed: " << error.what(); }
    }

    std::string getVampireHead(const ESM::RefId& race, bool female)
    {
        static std::map<std::pair<ESM::RefId, int>, const ESM::BodyPart*> sVampireMapping;

        std::pair<ESM::RefId, int> thisCombination = std::make_pair(race, int(female));

        if (sVampireMapping.find(thisCombination) == sVampireMapping.end())
        {
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            for (const ESM::BodyPart& bodypart : store.get<ESM::BodyPart>())
            {
                if (!bodypart.mData.mVampire)
                    continue;
                if (bodypart.mData.mType != ESM::BodyPart::MT_Skin)
                    continue;
                if (bodypart.mData.mPart != ESM::BodyPart::MP_Head)
                    continue;
                if (female != (bodypart.mData.mFlags & ESM::BodyPart::BPF_Female))
                    continue;
                if (!(bodypart.mRace == race))
                    continue;
                sVampireMapping[thisCombination] = &bodypart;
            }
        }

        sVampireMapping.emplace(thisCombination, nullptr);

        const ESM::BodyPart* bodyPart = sVampireMapping[thisCombination];
        if (!bodyPart)
            return std::string();
        return Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(bodyPart->mModel)).value();
    }

}

namespace MWRender
{
    class HeadAnimationTime : public SceneUtil::ControllerSource
    {
    private:
        MWWorld::Ptr mReference;
        float mTalkStart;
        float mTalkStop;
        float mBlinkStart;
        float mBlinkStop;

        float mBlinkTimer;

        bool mEnabled;

        float mValue;

    private:
        void resetBlinkTimer();

    public:
        HeadAnimationTime(const MWWorld::Ptr& reference);

        void updatePtr(const MWWorld::Ptr& updated);

        void update(float dt);

        void setEnabled(bool enabled);

        void setTalkStart(float value);
        void setTalkStop(float value);
        void setBlinkStart(float value);
        void setBlinkStop(float value);

        float getValue(osg::NodeVisitor* nv) override;
    };

    // --------------------------------------------------------------------------------------------------------------

    HeadAnimationTime::HeadAnimationTime(const MWWorld::Ptr& reference)
        : mReference(reference)
        , mTalkStart(0)
        , mTalkStop(0)
        , mBlinkStart(0)
        , mBlinkStop(0)
        , mEnabled(true)
        , mValue(0)
    {
        resetBlinkTimer();
    }

    void HeadAnimationTime::updatePtr(const MWWorld::Ptr& updated)
    {
        mReference = updated;
    }

    void HeadAnimationTime::setEnabled(bool enabled)
    {
        mEnabled = enabled;
    }

    void HeadAnimationTime::resetBlinkTimer()
    {
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        mBlinkTimer = -(2.0f + Misc::Rng::rollDice(6, prng));
    }

    void HeadAnimationTime::update(float dt)
    {
        if (!mEnabled)
            return;

        if (dt == 0.f)
            return;

        if (!MWBase::Environment::get().getSoundManager()->sayActive(mReference))
        {
            mBlinkTimer += dt;

            float duration = mBlinkStop - mBlinkStart;

            if (mBlinkTimer >= 0 && mBlinkTimer <= duration)
            {
                mValue = mBlinkStart + mBlinkTimer;
            }
            else
                mValue = mBlinkStop;

            if (mBlinkTimer > duration)
                resetBlinkTimer();
        }
        else
        {
            // FIXME: would be nice to hold on to the SoundPtr so we don't have to retrieve it every frame
            mValue = mTalkStart
                + (mTalkStop - mTalkStart)
                    * std::min(1.f,
                        MWBase::Environment::get().getSoundManager()->getSaySoundLoudness(mReference)
                            * 2); // Rescale a bit (most voices are not very loud)
        }
    }

    float HeadAnimationTime::getValue(osg::NodeVisitor*)
    {
        return mValue;
    }

    void HeadAnimationTime::setTalkStart(float value)
    {
        mTalkStart = value;
    }

    void HeadAnimationTime::setTalkStop(float value)
    {
        mTalkStop = value;
    }

    void HeadAnimationTime::setBlinkStart(float value)
    {
        mBlinkStart = value;
    }

    void HeadAnimationTime::setBlinkStop(float value)
    {
        mBlinkStop = value;
    }

    // ----------------------------------------------------

    NpcAnimation::NpcType NpcAnimation::getNpcType() const
    {
        const MWWorld::Class& cls = mPtr.getClass();
        // Dead vampires should typically stay vampires.
        if (mNpcType == Type_Vampire && cls.getNpcStats(mPtr).isDead() && !cls.getNpcStats(mPtr).isWerewolf())
            return mNpcType;
        return getNpcType(mPtr);
    }

    NpcAnimation::NpcType NpcAnimation::getNpcType(const MWWorld::Ptr& ptr)
    {
        const MWWorld::Class& cls = ptr.getClass();
        NpcAnimation::NpcType curType = Type_Normal;
        if (cls.getCreatureStats(ptr).getMagicEffects().getOrDefault(ESM::MagicEffect::Vampirism).getMagnitude() > 0)
            curType = Type_Vampire;
        if (cls.getNpcStats(ptr).isWerewolf())
            curType = Type_Werewolf;

        return curType;
    }

    static const inline NpcAnimation::PartBoneMap createPartListMap()
    {
        return { { ESM::PRT_Head, "Head" },
            { ESM::PRT_Hair, "Head" }, // note it uses "Head" as attach bone, but "Hair" as filter
            { ESM::PRT_Neck, "Neck" }, { ESM::PRT_Cuirass, "Chest" }, { ESM::PRT_Groin, "Groin" },
            { ESM::PRT_Skirt, "Groin" }, { ESM::PRT_RHand, "Right Hand" }, { ESM::PRT_LHand, "Left Hand" },
            { ESM::PRT_RWrist, "Right Wrist" }, { ESM::PRT_LWrist, "Left Wrist" }, { ESM::PRT_Shield, "Shield Bone" },
            { ESM::PRT_RForearm, "Right Forearm" }, { ESM::PRT_LForearm, "Left Forearm" },
            { ESM::PRT_RUpperarm, "Right Upper Arm" }, { ESM::PRT_LUpperarm, "Left Upper Arm" },
            { ESM::PRT_RFoot, "Right Foot" }, { ESM::PRT_LFoot, "Left Foot" }, { ESM::PRT_RAnkle, "Right Ankle" },
            { ESM::PRT_LAnkle, "Left Ankle" }, { ESM::PRT_RKnee, "Right Knee" }, { ESM::PRT_LKnee, "Left Knee" },
            { ESM::PRT_RLeg, "Right Upper Leg" }, { ESM::PRT_LLeg, "Left Upper Leg" },
            { ESM::PRT_RPauldron, "Right Clavicle" }, { ESM::PRT_LPauldron, "Left Clavicle" },
            { ESM::PRT_Weapon, "Weapon Bone" }, // Fallback. The real node name depends on the current weapon type.
            { ESM::PRT_Tail, "Tail" } };
    }
    const NpcAnimation::PartBoneMap NpcAnimation::sPartList = createPartListMap();

    struct NpcAnimation::GarmentCoverage
    {
        struct Callback : osg::NodeCallback
        {
            osg::observer_ptr<NpcAnimation> owner;
            explicit Callback(NpcAnimation* animation) : owner(animation) {}
            void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
            {
                traverse(node, visitor);
                osg::ref_ptr<NpcAnimation> animation;
                if (visitor && visitor->getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR
                    && owner.lock(animation))
                    animation->evaluateRagdollGarmentCoverage(*visitor);
            }
        };
        struct Surface
        {
            osg::ref_ptr<osg::Group> parent, wrapper;
            osg::ref_ptr<osg::Geometry> source;
            osg::ref_ptr<SceneUtil::BodyPartCoverage> clip;
            osg::NodePath path; // Exact instance path; never a shared Drawable's first parent.
        };
        struct Knee
        {
            ESM::PartReferenceType part;
            osg::NodePath anchorPath;
            osg::Vec3d distalAxis;
            std::vector<Surface> surfaces;
            SceneUtil::HemCoverage::Evaluation evaluation;
        };
        osg::observer_ptr<osg::Group> root;
        osg::ref_ptr<Callback> callback;
        osg::ref_ptr<SceneUtil::RigGeometry> skirt;
        osg::NodePath skirtPath;
        std::map<std::string, osg::NodePath> bonePaths;
        std::unique_ptr<SceneUtil::HemCoverage> hem;
        std::vector<Knee> knees;
        double gain = 0.;
        unsigned lastTraversal = ~0u, lastFrame = ~0u;

        ~GarmentCoverage()
        {
            osg::ref_ptr<osg::Group> node;
            if (root.lock(node) && callback) node->removeUpdateCallback(callback);
            for (auto& knee : knees)
                for (auto& surface : knee.surfaces)
                {
                    if (surface.clip) surface.clip->clearPlane();
                    // Restore before PartHolder detaches the original part.
                    if (surface.wrapper) surface.parent->replaceChild(surface.wrapper, surface.source);
                }
        }
    };

    struct NpcAnimation::GarmentAudit
    {
        struct Callback : osg::NodeCallback
        {
            osg::observer_ptr<NpcAnimation> owner;
            explicit Callback(NpcAnimation* animation) : owner(animation) {}
            void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
            {
                traverse(node, visitor);
                osg::ref_ptr<NpcAnimation> animation;
                if (visitor && visitor->getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR
                    && owner.lock(animation))
                {
                    animation->evaluateRagdollGarmentCoverage(*visitor);
                    animation->dumpRagdollGarmentForAudit(*visitor);
                }
            }
        };

        std::filesystem::path directory;
        osg::observer_ptr<osg::Group> root;
        osg::ref_ptr<Callback> callback;
        double animationElapsed = 0., ragdollStarted = 0.;
        bool livingSampled = false, ragdollSeen = false, disabled = false;
        unsigned nextRagdollSample = 0, samples = 0;
        unsigned lastTraversal = ~0u, lastFrame = ~0u;
        unsigned instance;
        long long session;
        static constexpr std::array<double, 6> targets{ 0., 0.03, 0.2, 0.6, 1.2, 2.5 };

        explicit GarmentAudit(std::filesystem::path path, NpcAnimation* owner)
            : directory(std::move(path)), callback(new Callback(owner))
        {
            static std::atomic<unsigned> sequence{ 0 };
            static const auto run = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            instance = ++sequence;
            session = run;
        }
        ~GarmentAudit()
        {
            osg::ref_ptr<osg::Group> node;
            if (root.lock(node))
                node->removeUpdateCallback(callback);
        }

        static void string(std::ostream& out, std::string_view value)
        {
            constexpr char hex[] = "0123456789abcdef";
            out << '"';
            for (const unsigned char c : value)
            {
                if (c == '"' || c == '\\') out << '\\' << char(c);
                else if (c < 0x20) out << "\\u00" << hex[c >> 4] << hex[c & 15];
                else out << char(c);
            }
            out << '"';
        }
        static void number(std::ostream& out, double value)
        {
            if (std::isfinite(value)) out << value;
            else out << "null";
        }
        static void vector(std::ostream& out, const osg::Vec3f& value)
        {
            out << '['; number(out, value.x()); out << ','; number(out, value.y());
            out << ','; number(out, value.z()); out << ']';
        }
        static void matrix(std::ostream& out, const osg::Matrixf& value)
        {
            out << '[';
            for (unsigned i = 0; i < 16; ++i)
            {
                if (i) out << ',';
                number(out, value.ptr()[i]);
            }
            out << ']';
        }
        static void geometry(std::ostream& out, const osg::Geometry& value)
        {
            out << "\"name\":"; string(out, value.getName());
            out << ",\"vertices\":[";
            const auto* vertices = dynamic_cast<const osg::Vec3Array*>(value.getVertexArray());
            if (vertices)
                for (std::size_t i = 0; i < vertices->size(); ++i)
                {
                    if (i) out << ',';
                    vector(out, (*vertices)[i]);
                }
            out << "],\"triangles\":[";
            osg::TriangleIndexFunctor<DecapitationGeometry::TriangleList> triangles;
            value.accept(triangles);
            for (std::size_t i = 0; i < triangles.triangles.size(); ++i)
            {
                if (i) out << ',';
                const auto& t = triangles.triangles[i];
                out << '[' << t[0] << ',' << t[1] << ',' << t[2] << ']';
            }
            out << "],\"normals\":[";
            const auto* normals = dynamic_cast<const osg::Vec3Array*>(value.getNormalArray());
            if (normals)
                for (std::size_t i = 0; i < normals->size(); ++i)
                {
                    if (i) out << ',';
                    vector(out, (*normals)[i]);
                }
            out << ']';
        }
    };

    void NpcAnimation::updateRagdollGarmentAudit(float timepassed)
    {
        // Read once: ordinary runs do not allocate state or add a scene callback.
        static const std::string directory = [] {
            const char* value = std::getenv("OPENMW_RAGDOLL_GARMENT_AUDIT");
            return value ? std::string(value) : std::string();
        }();
        if (directory.empty()) return;
        // Exclude player, inventory previews, and the body-awareness secondary rig.
        if (mBodyAwareness || mViewMode != VM_Normal || !mPtr.isInCell()
            || mPtr == MWMechanics::getPlayer() || mInsert.get() != mPtr.getRefData().getBaseNode())
            return;
        if (!mGarmentAudit)
        {
            if (!mObjectParts[ESM::PRT_Skirt]) return;
            mGarmentAudit = std::make_unique<GarmentAudit>(directory, this);
        }
        auto& audit = *mGarmentAudit;
        if (std::isfinite(timepassed) && timepassed > 0.f)
            audit.animationElapsed += timepassed;
        if (audit.disabled || audit.nextRagdollSample == audit.targets.size())
        {
            osg::ref_ptr<osg::Group> old;
            if (audit.root.lock(old)) old->removeUpdateCallback(audit.callback);
            audit.root = nullptr;
            return;
        }
        if (audit.root.get() != mObjectRoot.get())
        {
            osg::ref_ptr<osg::Group> old;
            if (audit.root.lock(old)) old->removeUpdateCallback(audit.callback);
            audit.root = mObjectRoot;
            if (mObjectRoot) mObjectRoot->addUpdateCallback(audit.callback);
        }
    }

    void NpcAnimation::dumpRagdollGarmentForAudit(osg::NodeVisitor& visitor)
    {
        if (!mGarmentAudit || mGarmentAudit->disabled) return;
        auto& audit = *mGarmentAudit;
        const auto* stamp = visitor.getFrameStamp();
        const unsigned frame = stamp ? stamp->getFrameNumber() : ~0u;
        if (audit.lastTraversal == visitor.getTraversalNumber() && audit.lastFrame == frame) return;
        audit.lastTraversal = visitor.getTraversalNumber();
        audit.lastFrame = frame;
        const bool ragdoll = isRagdollPose();
        if (!ragdoll && (audit.livingSampled || mPtr.getClass().getNpcStats(mPtr).isDead())) return;
        if (ragdoll && !audit.ragdollSeen)
        {
            audit.ragdollSeen = true;
            audit.ragdollStarted = audit.animationElapsed;
        }
        const double age = ragdoll ? audit.animationElapsed - audit.ragdollStarted : 0.;
        if (ragdoll && (audit.nextRagdollSample >= audit.targets.size()
                || age + 1e-7 < audit.targets[audit.nextRagdollSample])) return;
        const double target = ragdoll ? audit.targets[audit.nextRagdollSample] : 0.;
        try
        {
            if (!std::filesystem::is_directory(audit.directory))
                throw std::runtime_error("private audit directory does not exist");
            const auto path = audit.directory / ("garment-" + std::to_string(audit.session)
                + "-" + std::to_string(audit.instance)
                + "-sample-" + std::to_string(audit.samples) + "-frame-" + std::to_string(frame) + ".json");
            // Both selected parts and the matrix list use these exact evaluated
            // world matrices, rather than a physics pose or the cull-buffer cache.
            const std::function<std::optional<osg::Matrixf>(std::string_view)> bones
                = [this](std::string_view name) { return getBoneWorldMatrix(name); };
            std::ofstream out(path);
            if (!out) throw std::runtime_error("could not open private output");
            out.imbue(std::locale::classic());
            out << std::setprecision(std::numeric_limits<double>::max_digits10) << std::boolalpha;
            out << "{\"schema\":\"openmw-ragdoll-garment-audit\",\"version\":1,"
                "\"space\":\"world native units\",\"matrixConvention\":\"OSG row-vector; row-major ptr order\","
                "\"sampling\":\"post whole-object UPDATE traversal; same bones feed subsequent same-frame cull\","
                "\"session\":" << audit.session << ",\"instance\":" << audit.instance << ",\"sample\":" << audit.samples
                << ",\"frame\":" << frame << ",\"traversal\":" << visitor.getTraversalNumber()
                << ",\"frameStampSimulationTime\":";
            GarmentAudit::number(out, stamp ? stamp->getSimulationTime() : 0.);
            out << ",\"simulationTime\":";
            GarmentAudit::number(out, MWBase::Environment::get().getWorld()->getTimeManager()->getSimulationTime());
            out << ",\"animationElapsed\":"; GarmentAudit::number(out, audit.animationElapsed);
            out << ",\"ragdoll\":" << ragdoll << ",\"ragdollElapsed\":"; GarmentAudit::number(out, age);
            out << ",\"targetElapsed\":"; GarmentAudit::number(out, target);
            out << ",\"actorRecordId\":"; GarmentAudit::string(out, mPtr.getCellRef().getRefId().toString());
            out << ",\"actorRefNum\":"; GarmentAudit::string(out, mPtr.getCellRef().getRefNum().toString());
            out << ",\"actorScale\":"; GarmentAudit::number(out, mPtr.getCellRef().getScale());

            struct BoneNames : osg::NodeVisitor
            {
                std::set<std::string> names;
                BoneNames() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { setNodeMaskOverride(~0u); }
                void apply(osg::MatrixTransform& node) override
                {
                    if (node.getName().size() >= 5 && Misc::StringUtils::ciEqual(node.getName().substr(0, 5), "Bip01"))
                        names.insert(node.getName());
                    traverse(node);
                }
            } boneNames;
            mObjectRoot->accept(boneNames);
            out << ",\"bones\":[";
            bool firstBone = true;
            for (const auto& name : boneNames.names)
                if (const auto world = bones(name))
                {
                    if (!firstBone) out << ',';
                    firstBone = false;
                    out << "{\"name\":"; GarmentAudit::string(out, name);
                    out << ",\"worldMatrix\":"; GarmentAudit::matrix(out, *world); out << '}';
                }
            out << "],\"parts\":[";
            constexpr std::array selected{ ESM::PRT_Skirt, ESM::PRT_LKnee, ESM::PRT_RKnee };
            for (std::size_t i = 0; i < selected.size(); ++i)
            {
                const auto part = selected[i];
                if (i) out << ',';
                out << "{\"part\":" << int(part) << ",\"name\":";
                GarmentAudit::string(out, part == ESM::PRT_Skirt ? "skirt" : part == ESM::PRT_LKnee ? "leftKnee" : "rightKnee");
                out << ",\"model\":"; GarmentAudit::string(out, mPartModels[part]);
                out << ",\"equipmentSlot\":" << mPartslots[part] << ",\"equipmentRecordId\":";
                std::string equipped;
                auto& inventory = mPtr.getClass().getInventoryStore(mPtr);
                if (mPartslots[part] >= 0 && mPartslots[part] < MWWorld::InventoryStore::Slots)
                {
                    const auto item = inventory.getSlot(mPartslots[part]);
                    if (item != inventory.end()) equipped = item->getCellRef().getRefId().toString();
                }
                GarmentAudit::string(out, equipped);
                out << ",\"coverage\":";
                const GarmentCoverage::Knee* coveredKnee = nullptr;
                if (mGarmentCoverage)
                    for (const auto& knee : mGarmentCoverage->knees)
                        if (knee.part == part) coveredKnee = &knee;
                if (coveredKnee)
                {
                    const auto& value = coveredKnee->evaluation;
                    out << "{\"valid\":" << value.valid << ",\"worldPlane\":[";
                    for (unsigned k = 0; k < 4; ++k)
                    { if (k) out << ','; GarmentAudit::number(out, value.worldPlane[k]); }
                    out << "],\"appliedGain\":"; GarmentAudit::number(out, value.appliedGain);
                    out << ",\"footprintGain\":"; GarmentAudit::number(out, value.footprintGain);
                    out << ",\"hemClearance\":"; GarmentAudit::number(out, value.hemClearance);
                    out << ",\"visibleFraction\":"; GarmentAudit::number(out, value.visibleFraction);
                    out << '}';
                }
                else out << "null";
                std::vector<DecapitationGeometry::Snapshot> snapshots;
                DecapitationGeometry::SnapshotVisitor snapshot(snapshots, bones, osg::Matrixf::identity(), false);
                const osg::ref_ptr<osg::Node> root = mObjectParts[part] ? mObjectParts[part]->getNode() : nullptr;
                if (root) snapshot.snapshot(root);
                out << ",\"present\":" << bool(root) << ",\"nodeMask\":" << (root ? root->getNodeMask() : 0u)
                    << ",\"snapshotFailed\":" << snapshot.failed << ",\"geometries\":[";
                for (std::size_t j = 0; j < snapshots.size(); ++j)
                {
                    if (j) out << ',';
                    out << '{'; GarmentAudit::geometry(out, *snapshots[j].geometry); out << '}';
                }
                out << "]}";
            }
            out << "]}\n";
            out.flush();
            if (!out) throw std::runtime_error("could not finish private output");
            Log(Debug::Info) << "Ragdoll garment private audit: " << path;
            ++audit.samples;
            if (ragdoll) ++audit.nextRagdollSample;
            else audit.livingSampled = true;
        }
        catch (const std::exception& error)
        {
            audit.disabled = true;
            Log(Debug::Warning) << "Ragdoll garment audit output failed: " << error.what();
        }
    }

    void NpcAnimation::clearRagdollGarmentCoverage()
    {
        mGarmentCoverage.reset();
        mGarmentCoveragePrepared = false;
    }

    void NpcAnimation::prepareRagdollGarmentCoverage()
    {
        mGarmentCoveragePrepared = true;
        if (!mObjectParts[ESM::PRT_Skirt]
            || mPartslots[ESM::PRT_Skirt] != MWWorld::InventoryStore::Slot_Skirt)
            return;
        try
        {
            auto* actorRoot = mPtr.getRefData().getBaseNode();
            auto skirtRoot = mObjectParts[ESM::PRT_Skirt]->getNode();
            const auto skirtPath = releasedEquipmentPath(skirtRoot, actorRoot);
            DecapitationGeometry::RetainedRigVisitor garments;
            for (std::size_t i = 0; i + 1 < skirtPath.size(); ++i) garments.pushOntoNodePath(skirtPath[i]);
            skirtRoot->accept(garments);
            // Ambiguous/multipart hems retain their original rendering.
            if (garments.rigs.size() != 1) return;
            const auto& [rig, path] = garments.rigs.front();
            const auto source = rig->getSourceGeometry();
            if (!source || !source->getVertexArray()) return;
            std::set<unsigned> legVertices;
            for (const auto* name : { "Bip01 L Thigh", "Bip01 R Thigh", "Bip01 L Calf", "Bip01 R Calf" })
                for (const auto vertex : rig->getVerticesInfluencedBy(name)) legVertices.insert(vertex);
            std::vector<unsigned> waist;
            for (unsigned i = 0; i < source->getVertexArray()->getNumElements(); ++i)
                if (!legVertices.count(i)) waist.push_back(i);
            auto prepared = std::make_unique<GarmentCoverage>();
            prepared->hem = SceneUtil::HemCoverage::create(*source, waist);
            if (!prepared->hem) return;
            prepared->skirt = rig;
            prepared->skirtPath = path;
            for (const auto& name : rig->getInfluencingBoneNames())
            {
                const auto* node = getNode(name);
                if (!node) return;
                // The path helper validates equivalent shadow/view DAG paths and
                // rejects foreign/ambiguous actor transforms. It never mutates nodes.
                prepared->bonePaths.emplace(name, releasedEquipmentPath(const_cast<osg::Node*>(node), actorRoot));
            }

            struct RigidSurfaces : osg::NodeVisitor
            {
                std::vector<GarmentCoverage::Surface> surfaces;
                bool failed = false;
                RigidSurfaces() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { setNodeMaskOverride(~0u); }
                void apply(osg::Drawable& drawable) override
                {
                    auto* geometry = drawable.asGeometry();
                    const auto& path = getNodePath();
                    // Weighted/morphed/custom bodies require a different coverage mapping.
                    if (!geometry || (typeid(*geometry) != typeid(osg::Geometry) && !isRigidBloodSurface(geometry)) || path.size() < 2
                        || !dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray()))
                    { failed = true; return; }
                    auto* parent = path[path.size() - 2]->asGroup();
                    if (!parent || dynamic_cast<const osg::Billboard*>(parent)) { failed = true; return; }
                    GarmentCoverage::Surface surface;
                    surface.parent = parent;
                    surface.source = geometry;
                    surface.path = path;
                    surfaces.push_back(std::move(surface));
                }
            };
            for (const auto& [part, anchor, foot] : {
                     std::tuple{ ESM::PRT_LKnee, "Bip01 L Calf", "Bip01 L Foot" },
                     std::tuple{ ESM::PRT_RKnee, "Bip01 R Calf", "Bip01 R Foot" } })
            {
                if (!mObjectParts[part] || !mObjectParts[part]->getNode()->getNodeMask()) continue;
                auto* anchorNode = getNode(anchor);
                auto* footNode = getNode(foot);
                if (!anchorNode || !footNode) continue;
                auto partRoot = mObjectParts[part]->getNode();
                const auto ownPath = releasedEquipmentPath(partRoot, actorRoot);
                RigidSurfaces visitor;
                for (std::size_t i = 0; i + 1 < ownPath.size(); ++i) visitor.pushOntoNodePath(ownPath[i]);
                partRoot->accept(visitor);
                if (visitor.failed || visitor.surfaces.empty()) continue;
                std::vector<osg::Vec3d> localPoints;
                bool supported = true;
                for (const auto& surface : visitor.surfaces)
                {
                    const auto begin = std::find(surface.path.begin(), surface.path.end(), anchorNode);
                    if (begin == surface.path.end()) { supported = false; break; }
                    const osg::Matrixd matrix = osg::computeLocalToWorld(osg::NodePath(begin + 1, surface.path.end()));
                    const auto* vertices = static_cast<const osg::Vec3Array*>(surface.source->getVertexArray());
                    for (const auto& point : *vertices) localPoints.push_back(osg::Vec3d(point) * matrix);
                }
                const auto axis = SceneUtil::HemCoverage::principalAxis(localPoints);
                if (!supported || !axis) continue;
                const auto anchorPath = releasedEquipmentPath(const_cast<osg::Node*>(anchorNode), actorRoot);
                const auto footPath = releasedEquipmentPath(const_cast<osg::Node*>(footNode), actorRoot);
                const osg::Matrixd anchorWorld = osg::computeLocalToWorld(anchorPath);
                const osg::Matrixd footWorld = osg::computeLocalToWorld(footPath);
                auto distalAxis = *axis;
                if (osg::Matrixd::transform3x3(distalAxis, anchorWorld) * (footWorld.getTrans() - anchorWorld.getTrans()) < 0.)
                    distalAxis = -distalAxis;
                prepared->knees.push_back({part, anchorPath, distalAxis, std::move(visitor.surfaces), {}});
            }
            if (prepared->knees.empty()) return;
            for (auto& knee : prepared->knees)
                for (auto& surface : knee.surfaces)
                {
                    surface.wrapper = new osg::Group;
                    surface.wrapper->setName("Ragdoll garment knee coverage");
                    surface.clip = new SceneUtil::BodyPartCoverage;
                    surface.wrapper->addCullCallback(surface.clip);
                    surface.wrapper->addChild(surface.source);
                    if (!surface.parent->replaceChild(surface.source, surface.wrapper)) return;
                }
            prepared->root = mObjectRoot;
            prepared->callback = new GarmentCoverage::Callback(this);
            mObjectRoot->addUpdateCallback(prepared->callback);
            mGarmentCoverage = std::move(prepared);
        }
        catch (const std::runtime_error& error)
        {
            Log(Debug::Verbose) << "Ragdoll garment coverage uses original rendering: " << error.what();
        }
    }

    void NpcAnimation::updateRagdollGarmentCoverage(float timepassed)
    {
        // Private comparison control; ordinary runs use the enabled default.
        static const bool enabled = [] {
            const auto* value = std::getenv("OPENMW_RAGDOLL_GARMENT_COVERAGE");
            return !value || std::string_view(value) != "0";
        }();
        if (!enabled || !Settings::game().mRagdollEnhancement || mBisection || mLimbSevering || mBodyAwareness || mViewMode != VM_Normal
            || !mPtr.isInCell() || mPtr == MWMechanics::getPlayer()
            || mInsert.get() != mPtr.getRefData().getBaseNode() || (!mRagdollPose && !mRagdollRecoveryProgress))
        {
            clearRagdollGarmentCoverage();
            mGarmentRagdollAge = 0.;
            mGarmentWasRagdoll = false;
            return;
        }
        if (mRagdollPose)
        {
            if (!mGarmentWasRagdoll) mGarmentRagdollAge = 0.;
            if (std::isfinite(timepassed)) mGarmentRagdollAge += std::max(0.f, timepassed);
        }
        mGarmentWasRagdoll = mRagdollPose;
        if (!mGarmentCoveragePrepared) prepareRagdollGarmentCoverage();
        if (!mGarmentCoverage) return;
        const auto smooth = [](double t) { t = std::clamp(t, 0., 1.); return t * t * (3. - 2. * t); };
        mGarmentCoverage->gain = smooth(mGarmentRagdollAge / .1)
            * (mRagdollRecoveryProgress ? 1. - smooth(*mRagdollRecoveryProgress) : 1.);
    }

    void NpcAnimation::evaluateRagdollGarmentCoverage(osg::NodeVisitor& visitor)
    {
        if (!mGarmentCoverage) return;
        auto& coverage = *mGarmentCoverage;
        const unsigned frame = visitor.getFrameStamp() ? visitor.getFrameStamp()->getFrameNumber() : ~0u;
        if (coverage.lastTraversal == visitor.getTraversalNumber() && coverage.lastFrame == frame) return;
        coverage.lastTraversal = visitor.getTraversalNumber();
        coverage.lastFrame = frame;
        for (auto& knee : coverage.knees)
        {
            knee.evaluation = {};
            for (auto& surface : knee.surfaces) surface.clip->clearPlane();
        }
        const std::function<std::optional<osg::Matrixf>(std::string_view)> bones
            = [&coverage](std::string_view name) -> std::optional<osg::Matrixf> {
                const auto found = coverage.bonePaths.find(std::string(name));
                if (found == coverage.bonePaths.end()) return std::nullopt;
                return osg::Matrixf(osg::computeLocalToWorld(found->second));
            };
        const auto skirt = coverage.skirt->snapshotWorldGeometry(coverage.skirtPath, bones, true);
        const auto* vertices = skirt ? dynamic_cast<const osg::Vec3Array*>(skirt->getVertexArray()) : nullptr;
        if (!vertices || vertices->size() != coverage.hem->getSourceVertexCount()) return;
        std::vector<osg::Vec3d> hemWorld;
        for (const auto& aliases : coverage.hem->getHemVertices())
        {
            if (aliases.empty() || aliases.front() >= vertices->size()) return;
            const osg::Vec3d position((*vertices)[aliases.front()]);
            for (unsigned index : aliases)
                if (index >= vertices->size() || (osg::Vec3d((*vertices)[index]) - position).length2() > 1e-6)
                    return;
            hemWorld.push_back(position);
        }
        for (auto& knee : coverage.knees)
        {
            std::vector<osg::Vec3d> points;
            std::vector<osg::Matrixd> matrices;
            for (const auto& surface : knee.surfaces)
            {
                const osg::Matrixd matrix = osg::computeLocalToWorld(surface.path);
                matrices.push_back(matrix);
                for (const auto& point : *static_cast<const osg::Vec3Array*>(surface.source->getVertexArray()))
                    points.push_back(osg::Vec3d(point) * matrix);
            }
            const osg::Matrixd anchorWorld = osg::computeLocalToWorld(knee.anchorPath);
            knee.evaluation = coverage.hem->evaluate(hemWorld, points,
                osg::Matrixd::transform3x3(knee.distalAxis, anchorWorld), coverage.gain);
            if (!knee.evaluation.valid || knee.evaluation.appliedGain <= 0.) continue;
            for (std::size_t s = 0; s < knee.surfaces.size(); ++s)
            {
                osg::Vec4d localPlane;
                // OSG uses row-vector points. Transform the plane column by M,
                // retaining the keep-positive halfspace even for reflected parts.
                for (unsigned i = 0; i < 4; ++i)
                    for (unsigned j = 0; j < 4; ++j)
                        localPlane[i] += matrices[s](i, j) * knee.evaluation.worldPlane[j];
                knee.surfaces[s].clip->setPlane(localPlane);
            }
        }
    }

    NpcAnimation::~NpcAnimation()
    {
        clearCutBloodFlow();
        mBisection.reset();
        mLimbSevering.reset();
        clearRagdollGarmentCoverage();
        mGarmentAudit.reset();
        resetBodyAwareness();
        mAmmunition.reset();
    }

    NpcAnimation::NpcAnimation(const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode,
        Resource::ResourceSystem* resourceSystem, bool disableSounds, ViewMode viewMode, float firstPersonFieldOfView,
        bool bodyAwareness)
        : ActorAnimation(ptr, std::move(parentNode), resourceSystem)
        , mViewMode(viewMode)
        , mShowWeapons(false)
        , mShowCarriedLeft(true)
        , mNpcType(getNpcType(ptr))
        , mFirstPersonFieldOfView(firstPersonFieldOfView)
        , mSoundsDisabled(disableSounds)
        , mBodyAwareness(bodyAwareness)
        , mAccurateAiming(false)
        , mAimingFactor(0.f)
        , mHazMangchu(false)
        , mHazFormCode(-1)
        , mHazHeadRoll(0)
        , mHazHairRoll(0)
    {
        mNpc = mPtr.get<ESM::NPC>()->mBase;

        mHeadAnimationTime = std::make_shared<HeadAnimationTime>(mPtr);
        mWeaponAnimationTime = std::make_shared<WeaponAnimationTime>(this);

        for (size_t i = 0; i < ESM::PRT_Count; i++)
        {
            mPartslots[i] = -1; // each slot is empty
            mPartPriorities[i] = 0;
        }

        std::fill(mSounds.begin(), mSounds.end(), nullptr);

        updateNpcBase();
    }

    bool NpcAnimation::useFullBodyFirstPerson() const
    {
        return mViewMode == VM_FirstPerson && Settings::game().mFirstPersonFullBody
            && mPtr == MWMechanics::getPlayer();
    }

    void NpcAnimation::setViewMode(NpcAnimation::ViewMode viewMode)
    {
        assert(viewMode != VM_HeadOnly);
        if (mViewMode == viewMode)
            return;
        // FIXME: sheathing state must be consistent if the third person skeleton doesn't have the necessary node, but
        // third person skeleton is unavailable in first person view. This is a hack to avoid cosmetic issues.
        bool viewChange = mViewMode == VM_FirstPerson || viewMode == VM_FirstPerson;
        mViewMode = viewMode;
        MWBase::Environment::get().getWorld()->scaleObject(
            mPtr, mPtr.getCellRef().getScale(), true); // apply race height after view change

        mAmmunition.reset();
        rebuild();
        setRenderBin();

        if (viewChange && Settings::game().mShieldSheathing)
        {
            int weaptype = ESM::Weapon::None;
            MWMechanics::getActiveWeapon(mPtr, &weaptype);
            showCarriedLeft(updateCarriedLeftVisible(weaptype));
        }
    }

    /// @brief A RenderBin callback to clear the depth buffer before rendering.
    /// Switches depth attachments to a proxy renderbuffer, reattaches original depth then redraws first person root.
    /// This gives a complete depth buffer which can be used for postprocessing, buffer resolves as if depth was never
    /// cleared.
    class DepthClearCallback : public osgUtil::RenderBin::DrawCallback
    {
    public:
        DepthClearCallback()
        {
            mDepth = new SceneUtil::AutoDepth;
            mDepth->setWriteMask(true);

            mStateSet = new osg::StateSet;
            mStateSet->setAttributeAndModes(new osg::ColorMask(false, false, false, false), osg::StateAttribute::ON);
        }

        void drawImplementation(
            osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous) override
        {
            osg::State* state = renderInfo.getState();

            PostProcessor* postProcessor = static_cast<PostProcessor*>(renderInfo.getCurrentCamera()->getUserData());

            state->applyAttribute(mDepth);

            unsigned int frameId = state->getFrameStamp()->getFrameNumber() % 2;

            postProcessor->getFbo(PostProcessor::FBO_FirstPerson, frameId)->apply(*state);
            glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            // color accumulation pass
            bin->drawImplementation(renderInfo, previous);

            auto primaryFBO = postProcessor->getPrimaryFbo(frameId);
            primaryFBO->apply(*state);

            postProcessor->getFbo(PostProcessor::FBO_OpaqueDepth, frameId)->apply(*state);

            // depth accumulation pass
            osg::ref_ptr<osg::StateSet> restore = bin->getStateSet();
            bin->setStateSet(mStateSet);
            bin->drawImplementation(renderInfo, previous);
            bin->setStateSet(restore);

            primaryFBO->apply(*state);

            state->checkGLErrors("after DepthClearCallback::drawImplementation");
        }

        osg::ref_ptr<osg::Depth> mDepth;
        osg::ref_ptr<osg::StateSet> mStateSet;
    };

    class BodyAwarenessClinchCullCallback : public osg::NodeCallback
    {
        osg::observer_ptr<NpcAnimation> mOwner;
    public:
        explicit BodyAwarenessClinchCullCallback(NpcAnimation* owner) : mOwner(owner) {}
        void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
        {
            osg::ref_ptr<NpcAnimation> owner;
            if (mOwner.lock(owner) && owner->isBodyAwarenessHiddenForClinch())
                return;
            traverse(node, visitor);
        }
    };

    /// Overrides Field of View to given value for rendering the subgraph.
    /// Must be added as cull callback.
    class OverrideFieldOfViewCallback : public osg::NodeCallback
    {
    public:
        OverrideFieldOfViewCallback(float fov, RenderingManager* renderingManager)
            : mFov(fov)
            , mRenderingManager(renderingManager)
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);
            float fov, aspect, zNear, zFar;
            if (cv->getProjectionMatrix()->getPerspective(fov, aspect, zNear, zFar) && std::abs(fov - mFov) > 0.001)
            {
                fov = mFov;
                osg::ref_ptr<osg::RefMatrix> newProjectionMatrix = new osg::RefMatrix();
                newProjectionMatrix->makePerspective(fov, aspect, zNear, zFar);

                osg::Vec2f offset = mRenderingManager->getProjectionOffset();

                double offsetX = (offset.x() / cv->getViewport()->width()) * 2.0;
                double offsetY = (offset.y() / cv->getViewport()->height()) * 2.0;

                const osg::Matrix translation = osg::Matrix::translate(offsetX, offsetY, 0.0);
                newProjectionMatrix->postMult(translation);

                osg::ref_ptr<osg::RefMatrix> invertedOldMatrix = cv->getProjectionMatrix();
                invertedOldMatrix = new osg::RefMatrix(osg::RefMatrix::inverse(*invertedOldMatrix));
                osg::ref_ptr<osg::RefMatrix> viewMatrix = new osg::RefMatrix(*cv->getModelViewMatrix());
                viewMatrix->postMult(*newProjectionMatrix);
                viewMatrix->postMult(*invertedOldMatrix);
                cv->pushModelViewMatrix(viewMatrix, osg::Transform::ReferenceFrame::ABSOLUTE_RF);
                traverse(node, nv);
                cv->popModelViewMatrix();
            }
            else
                traverse(node, nv);
        }

    private:
        float mFov;
        RenderingManager* mRenderingManager;
    };

    void NpcAnimation::setRenderBin()
    {
        if (mViewMode == VM_FirstPerson && !useFullBodyFirstPerson())
        {
            [[maybe_unused]] static const bool prototypeAdded = [&] {
                osg::ref_ptr<osgUtil::RenderBin> depthClearBin(new osgUtil::RenderBin);
                depthClearBin->setDrawCallback(new DepthClearCallback());
                osgUtil::RenderBin::addRenderBinPrototype("DepthClear", depthClearBin);
                return true;
            }();
            mObjectRoot->getOrCreateStateSet()->setRenderBinDetails(
                RenderBin_FirstPerson, "DepthClear", osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);
        }
        else if (osg::StateSet* stateset = mObjectRoot->getStateSet())
            stateset->setRenderBinToInherit();
    }

    void NpcAnimation::rebuild()
    {
        mScabbard.reset();
        mHolsteredShield.reset();
        updateNpcBase();

        if (!mBodyAwareness)
            MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(mPtr);
    }

    int NpcAnimation::getSlot(const osg::NodePath& path) const
    {
        for (int i = 0; i < ESM::PRT_Count; ++i)
        {
            const PartHolder* const part = mObjectParts[i].get();
            if (part == nullptr)
                continue;
            if (std::find(path.begin(), path.end(), part->getNode().get()) != path.end())
            {
                return mPartslots[i];
            }
        }
        return -1;
    }

    ESM::RefId NpcAnimation::hazFormRace(int code)
    {
        // PC_hz_ht form codes -> race ids, matching the Hazaeki Shapeshifter Race mod's controller
        // (SG_HAZ_globolscript). 0 (and any unmapped code) means "no override": the player is shown
        // as their true Hazaeki self. Creature forms never set PC_hz_ht, so they never reach here.
        switch (code)
        {
            case 1:
                return ESM::RefId::stringRefId("Argonian");
            case 2:
                return ESM::RefId::stringRefId("Breton");
            case 4:
                return ESM::RefId::stringRefId("High Elf");
            case 5:
                return ESM::RefId::stringRefId("Imperial");
            case 6:
                return ESM::RefId::stringRefId("Khajiit");
            case 7:
                return ESM::RefId::stringRefId("Nord");
            case 8:
                return ESM::RefId::stringRefId("Orc");
            case 9:
                return ESM::RefId::stringRefId("Redguard");
            case 10:
                return ESM::RefId::stringRefId("Wood Elf");
            case 11:
                // Drow City Denizens.esp (a master of the mod) defines this race with a full skin set.
                return ESM::RefId::stringRefId("Ddrow");
            case 12:
                return ESM::RefId::stringRefId("Anen Nirrera");
            // 13 (Mang'Chu) has no race - its skin ships as clothing-tagged parts and is handled
            // separately via buildMangchuBodyParts(); see updateBodyRaceOverride().
            default:
                return ESM::RefId();
        }
    }

    std::vector<const ESM::BodyPart*> NpcAnimation::buildMangchuBodyParts(bool female) const
    {
        // The Mang'Chu form's skin ships in the mod as body parts tagged type=clothing, race=Hazaeki
        // (delivered through the "_sg_hz_tng_*" clothing), so getBodyParts() by race can't find them.
        // Resolve them by explicit id into the same PRT_-indexed layout getBodyParts() returns, so the
        // naked-body attach loop in updateParts() renders them exactly like any other race's skin.
        struct Entry
        {
            ESM::PartReferenceType mSlot;
            std::string_view mMale;
            std::string_view mFemale;
        };
        static const Entry entries[] = {
            { ESM::PRT_Neck, "_sg_hz_tng_m_neck", "_sg_hz_tng_f_neck" },
            { ESM::PRT_Cuirass, "_sg_hz_tng_m_chest", "_sg_hz_tng_f_chest" },
            { ESM::PRT_Groin, "_sg_hz_tng_m_groin", "_sg_hz_tng_f_groin" },
            { ESM::PRT_RHand, "_sg_hz_tng_m_hands", "_sg_hz_tng_f_hands" },
            { ESM::PRT_LHand, "_sg_hz_tng_m_hands", "_sg_hz_tng_f_hands" },
            { ESM::PRT_RWrist, "_sg_hz_tng_m_wrist", "_sg_hz_tng_f_wrist" },
            { ESM::PRT_LWrist, "_sg_hz_tng_m_wrist", "_sg_hz_tng_f_wrist" },
            { ESM::PRT_RForearm, "_sg_hz_tng_m_forearm", "_sg_hz_tng_f_forearm" },
            { ESM::PRT_LForearm, "_sg_hz_tng_m_forearm", "_sg_hz_tng_f_forearm" },
            { ESM::PRT_RUpperarm, "_sg_hz_tng_m_upper_arm", "_sg_hz_tng_f_upper_arm" },
            { ESM::PRT_LUpperarm, "_sg_hz_tng_m_upper_arm", "_sg_hz_tng_f_upper_arm" },
            { ESM::PRT_RFoot, "_sg_hz_tng_m_feet", "_sg_hz_tng_f_feet" },
            { ESM::PRT_LFoot, "_sg_hz_tng_m_feet", "_sg_hz_tng_f_feet" },
            { ESM::PRT_RAnkle, "_sg_hz_tng_m_ankle", "_sg_hz_tng_f_ankle" },
            { ESM::PRT_LAnkle, "_sg_hz_tng_m_ankle", "_sg_hz_tng_f_ankle" },
            { ESM::PRT_RKnee, "_sg_hz_tng_m_knee", "_sg_hz_tng_f_knee" },
            { ESM::PRT_LKnee, "_sg_hz_tng_m_knee", "_sg_hz_tng_f_knee" },
            { ESM::PRT_RLeg, "_sg_hz_tng_m_upper_leg", "_sg_hz_tng_f_upper_leg" },
            { ESM::PRT_LLeg, "_sg_hz_tng_m_upper_leg", "_sg_hz_tng_f_upper_leg" },
        };

        std::vector<const ESM::BodyPart*> parts(ESM::PRT_Count, nullptr);
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        for (const Entry& e : entries)
        {
            const ESM::BodyPart* bp = store.get<ESM::BodyPart>().search(
                ESM::RefId::stringRefId(female ? e.mFemale : e.mMale));
            if (bp)
                parts[e.mSlot] = bp;
        }
        return parts;
    }

    ESM::RefId NpcAnimation::displayBodyRace() const
    {
        return mBodyRaceOverride.empty() ? mNpc->mRace : mBodyRaceOverride;
    }

    VFS::Path::Normalized NpcAnimation::findRaceHeadOrHair(const ESM::RefId& race, bool female, bool hair) const
    {
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM::BodyPart::MeshPart wanted = hair ? ESM::BodyPart::MP_Hair : ESM::BodyPart::MP_Head;
        // Collect every candidate so the form can pick a random head/hair (as the mod used to via its
        // random clothing variants). Same-sex parts are preferred, opposite-sex as a fallback set.
        std::vector<const ESM::BodyPart*> sameSex;
        std::vector<const ESM::BodyPart*> otherSex;
        for (const ESM::BodyPart& bp : store.get<ESM::BodyPart>())
        {
            if (bp.mData.mFlags & ESM::BodyPart::BPF_NotPlayable)
                continue;
            if (bp.mData.mType != ESM::BodyPart::MT_Skin)
                continue;
            if (bp.mData.mPart != wanted)
                continue;
            if (!(bp.mRace == race))
                continue;
            if (ESM::isFirstPersonBodyPart(bp))
                continue;
            (isFemalePart(&bp) == female ? sameSex : otherSex).push_back(&bp);
        }
        const std::vector<const ESM::BodyPart*>& pick = !sameSex.empty() ? sameSex : otherSex;
        if (pick.empty())
            return {};
        // Head and hair use independent rolls, so any head can pair with any hair (not fixed pairs).
        const std::size_t index = static_cast<std::size_t>(hair ? mHazHairRoll : mHazHeadRoll) % pick.size();
        return Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(pick[index]->mModel));
    }

    void NpcAnimation::updateBodyRaceOverride()
    {
        // Only the player is ever transformed by the mod.
        if (mPtr != MWMechanics::getPlayer())
            return;

        const MWBase::World* world = MWBase::Environment::get().getWorld();

        // string_view over string literals: static lifetime, so GlobalVariableName's stored view does
        // not dangle (and this selects the public string_view ctor, not the private const char* one).
        constexpr std::string_view pcHazaeki = "pc_hazaeki";
        constexpr std::string_view pcHzHt = "pc_hz_ht";

        // Reading a non-existent global throws, so probe the type first. The override only applies
        // to a Hazaeki PC (PC_hazaeki != 0) who is in a humanoid form (PC_hz_ht != 0).
        int code = 0;
        if (world->getGlobalVariableType(MWWorld::GlobalVariableName{ pcHazaeki }) != ' '
            && world->getGlobalInt(MWWorld::GlobalVariableName{ pcHazaeki }) != 0
            && world->getGlobalVariableType(MWWorld::GlobalVariableName{ pcHzHt }) != ' ')
        {
            code = static_cast<int>(world->getGlobalFloat(MWWorld::GlobalVariableName{ pcHzHt }));
        }

        if (code == mHazFormCode)
            return; // unchanged since the last frame - avoid needless rebuilds

        ESM::RefId race = hazFormRace(code);
        bool mangchu = (code == 13);

        // Only override when the target actually has skin parts loaded; otherwise leave the real race
        // in place so an unmapped/unavailable form can't make the body vanish.
        if (!race.empty())
        {
            bool hasParts = false;
            for (const ESM::BodyPart* p : getBodyParts(race, !mNpc->isMale(), false, false))
            {
                if (p)
                {
                    hasParts = true;
                    break;
                }
            }
            if (!hasParts)
                race = ESM::RefId();
        }
        if (mangchu)
        {
            bool hasParts = false;
            for (const ESM::BodyPart* p : buildMangchuBodyParts(!mNpc->isMale()))
            {
                if (p)
                {
                    hasParts = true;
                    break;
                }
            }
            mangchu = hasParts;
        }

        mHazFormCode = code;
        if (race == mBodyRaceOverride && mangchu == mHazMangchu)
            return; // resolves to the same appearance - no rebuild needed

        mBodyRaceOverride = race;
        mHazMangchu = mangchu;
        // Re-roll head and hair independently on each form change (mirrors the mod's random pick, but
        // decoupled so any head can pair with any hair rather than fixed head/hair pairs).
        if (!race.empty() || mangchu)
        {
            mHazHeadRoll = Misc::Rng::rollDice(1000);
            mHazHairRoll = Misc::Rng::rollDice(1000);
        }
        rebuild();
    }

    void NpcAnimation::updateNpcBase()
    {
        clearRagdollGarmentCoverage();
        resetBodyAwareness();
        clearAnimSources();
        for (size_t i = 0; i < ESM::PRT_Count; i++)
            removeIndividualPart((ESM::PartReferenceType)i);

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM::Race* race = store.get<ESM::Race>().find(mNpc->mRace);
        NpcType curType = getNpcType();
        bool isWerewolf = (curType == Type_Werewolf);
        bool isVampire = (curType == Type_Vampire);
        bool isFemale = !mNpc->isMale();

        mHeadModel.clear();
        mHairModel.clear();

        const ESM::RefId headName = isWerewolf ? ESM::RefId::stringRefId("WerewolfHead") : mNpc->mHead;
        const ESM::RefId hairName = isWerewolf ? ESM::RefId::stringRefId("WerewolfHair") : mNpc->mHair;

        if (!headName.empty())
        {
            const ESM::BodyPart* bp = store.get<ESM::BodyPart>().search(headName);
            if (bp)
                mHeadModel = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(bp->mModel));
            else
                Log(Debug::Warning) << "Warning: Failed to load body part '" << headName << "'";
        }

        if (!hairName.empty())
        {
            const ESM::BodyPart* bp = store.get<ESM::BodyPart>().search(hairName);
            if (bp)
                mHairModel = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(bp->mModel));
            else
                Log(Debug::Warning) << "Warning: Failed to load body part '" << hairName << "'";
        }

        // Hazaeki humanoid form: display the target race's head & hair in place of the PC's own, so
        // the transformed appearance is complete. Keeps the PC's own part if the form lacks one.
        if (!isWerewolf && !mBodyRaceOverride.empty())
        {
            VFS::Path::Normalized overrideHead = findRaceHeadOrHair(mBodyRaceOverride, isFemale, false);
            if (!overrideHead.empty())
                mHeadModel = overrideHead;
            VFS::Path::Normalized overrideHair = findRaceHeadOrHair(mBodyRaceOverride, isFemale, true);
            if (!overrideHair.empty())
                mHairModel = overrideHair;
        }
        else if (!isWerewolf && mHazMangchu)
        {
            // Mang'Chu head & hair are clothing-tagged parts (no race), so gather all "_sg_hz_tng_*"
            // head/hair variants by id prefix and pick one with the per-form random roll.
            const auto pickByPrefix = [&](std::string_view prefix, int roll) -> VFS::Path::Normalized {
                std::vector<const ESM::BodyPart*> cands;
                for (const ESM::BodyPart& bp : store.get<ESM::BodyPart>())
                    if (Misc::StringUtils::ciStartsWith(bp.mId.getRefIdString(), prefix))
                        cands.push_back(&bp);
                if (cands.empty())
                    return {};
                const std::size_t index = static_cast<std::size_t>(roll) % cands.size();
                return Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(cands[index]->mModel));
            };
            VFS::Path::Normalized head = pickByPrefix(isFemale ? "_sg_hz_tng_f_head_" : "_sg_hz_tng_m_head_", mHazHeadRoll);
            if (!head.empty())
                mHeadModel = head;
            VFS::Path::Normalized hair = pickByPrefix(isFemale ? "_sg_hz_tng_f_hair_" : "_sg_hz_tng_m_hair_", mHazHairRoll);
            if (!hair.empty())
                mHairModel = hair;
        }

        const std::string vampireHead = getVampireHead(mNpc->mRace, isFemale);
        if (!isWerewolf && isVampire && !vampireHead.empty())
            mHeadModel = vampireHead;

        bool is1stPerson = mViewMode == VM_FirstPerson && !useFullBodyFirstPerson();
        bool isBeast = (race->mData.mFlags & ESM::Race::Beast) != 0;

        std::string_view base;
        if (!isWerewolf)
        {
            if (!is1stPerson)
                base = Settings::models().mXbaseanim.get().value();
            else
                base = Settings::models().mXbaseanim1st.get().value();
        }

        const std::string defaultSkeleton = Misc::ResourceHelpers::correctActorModelPath(
            VFS::Path::toNormalized(getActorSkeleton(is1stPerson, isFemale, isBeast, isWerewolf)),
            mResourceSystem->getVFS());

        // The Dark Messiah viewmodel rig and clips live under their own paths, so that
        // classic mode reaches the original first-person skeleton and animations.
        const bool brutalPlayerArms = is1stPerson && !mBodyAwareness && mPtr == MWMechanics::getPlayer()
            && MWMechanics::brutalDmFirstPerson();
        constexpr VFS::Path::NormalizedView brutalSkeleton("meshes/brutal_fp/xbase_anim.1st.nif");
        const bool brutalRig = brutalPlayerArms && mResourceSystem->getVFS()->exists(brutalSkeleton);
        std::string smodel = brutalRig ? std::string(brutalSkeleton.value()) : defaultSkeleton;
        bool isCustomModel = false;
        if (!is1stPerson && !isWerewolf && !mNpc->mModel.empty())
        {
            VFS::Path::Normalized model = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(mNpc->mModel));
            isCustomModel = !isDefaultActorSkeleton(model);
            smodel = Misc::ResourceHelpers::correctActorModelPath(model, mResourceSystem->getVFS());
        }

        // Body jiggle is female-only. Which females jiggle (the player and listed NPCs) is decided per
        // bone in attachJiggleBoneControllers(); this also lets Verlet cloth chains attach for everyone.
        setObjectRoot(smodel, true, true, false, isFemale);

        mViewmodelSet = is1stPerson && mPtr == MWMechanics::getPlayer() && MWMechanics::brutalDmFirstPerson()
            ? brutalViewmodelSet() : BrutalViewmodel_Default;
        updateParts();

        if (!base.empty())
            addAnimSource(base, smodel);

        if (defaultSkeleton != base)
            addAnimSource(defaultSkeleton, smodel);

        if (isCustomModel)
            addAnimSource(smodel, smodel);

        const bool customArgonianSwim = !is1stPerson && !isWerewolf && isBeast && mNpc->mRace.contains("argonian");
        if (customArgonianSwim)
            addAnimSource(Settings::models().mXargonianswimkna.get().value(), smodel);

        if (brutalRig)
        {
            loadAdditionalAnimations(VFS::Path::NormalizedView("meshes/brutal_fp.nif"), smodel);
            if (mViewmodelSet == BrutalViewmodel_Shield)
                loadAdditionalAnimations(VFS::Path::NormalizedView("meshes/brutal_fp_shield.nif"), smodel);
            else if (mViewmodelSet == BrutalViewmodel_Staff)
                loadAdditionalAnimations(VFS::Path::NormalizedView("meshes/brutal_fp_staff.nif"), smodel);
            else if (mViewmodelSet == BrutalViewmodel_Dagger)
                loadAdditionalAnimations(VFS::Path::NormalizedView("meshes/brutal_fp_dagger.nif"), smodel);
        }

        if (is1stPerson)
        {
            mObjectRoot->setNodeMask(Mask_FirstPerson);
            mObjectRoot->addCullCallback(new OverrideFieldOfViewCallback(
                mFirstPersonFieldOfView, MWBase::Environment::get().getWorld()->getRenderingManager()));
        }

        // Separate player and NPC paths keep their last-priority overrides isolated.
        // The helper maps this virtual model path to animations/brutal_firstperson/*.kf.
        if ((useFullBodyFirstPerson() || mBodyAwareness) && MWMechanics::brutalDmFirstPerson())
            loadAdditionalAnimations(VFS::Path::NormalizedView("meshes/brutal_firstperson.nif"), smodel);
        else if (!isWerewolf && mPtr != MWMechanics::getPlayer() && Settings::game().mUseAdditionalAnimSources
            && !Settings::game().mBrutalClassicNpcAnimations)
            loadAdditionalAnimations(VFS::Path::NormalizedView("meshes/brutal_npc.nif"), smodel);

        mWeaponAnimationTime->updateStartTime();
        updateBodyAwareness();
    }

    void NpcAnimation::resetBodyAwareness()
    {
        mBodyAwarenessHiddenForClinch = false;
        mBodyAwarenessAnimation = nullptr;
        if (mBodyAwarenessRoot)
            mInsert->removeChild(mBodyAwarenessRoot);
        mBodyAwarenessRoot = nullptr;
        mBodyGroundNormal = mBodyGroundUp = osg::Vec3f(0.f, 0.f, 1.f);
        mBodyGroundOffset = mBodyGroundLift = mBodyGroundBlend = 0.f;
        mBodyGroundAnchor = osg::Vec3f();
    }

    void NpcAnimation::updateBodyAwareness()
    {
        const bool enabled = !mBodyAwareness && mViewMode == VM_FirstPerson && !useFullBodyFirstPerson()
            && Settings::game().mBrutalEnabled && Settings::game().mBrutalFirstPersonBody && mPtr == MWMechanics::getPlayer();
        if (!enabled)
        {
            resetBodyAwareness();
            return;
        }
        if (mBodyAwarenessAnimation)
            return;

        mBodyAwarenessRoot = new osg::MatrixTransform;
        mBodyAwarenessRoot->setName("First Person Body Awareness");
        mBodyAwarenessRoot->setNodeMask(Mask_Player);
        mBodyAwarenessRoot->addCullCallback(new BodyAwarenessClinchCullCallback(this));
        mInsert->addChild(mBodyAwarenessRoot);
        mBodyAwarenessAnimation = new NpcAnimation(mPtr, mBodyAwarenessRoot, mResourceSystem,
            true, VM_Normal, mFirstPersonFieldOfView, true);
        mBodyAwarenessAnimation->enableHeadAnimation(false);
        mBodyAwarenessAnimation->setActive(2);
    }

    void NpcAnimation::alignBodyAwareness(float duration)
    {
        const osg::Matrixd mount = osg::Matrixd::translate(
            0., -Settings::game().mFirstPersonBodyForward.get(), 0.);
        mBodyAwarenessRoot->setMatrix(mount);
        const auto paths = mInsert->getParentalNodePaths();
        if (paths.empty())
            return;
        const osg::Matrixd parentWorld = osg::computeLocalToWorld(paths.front());
        const osg::Vec3d origin = parentWorld.getTrans();
        const auto& stats = mPtr.getClass().getCreatureStats(mPtr);
        // Headroom can retain the camera's low blend after the authored slide has
        // ended. Never carry its ground tilt into an unrelated crouch/idle pose.
        const bool slidePose = !stats.isDead() && stats.mBrutalSlideLow && stats.mBrutalSlidePhase != 0;
        float blend = slidePose ? std::clamp(stats.mBrutalSlideBlend, 0.f, 1.f) : 0.f;
        // Match the authored body's leading smoothstep, including its zero-velocity
        // endpoints. Camera lowering has its own linear blend and stays independent.
        blend = blend * blend * (3.f - 2.f * blend);
        mBodyGroundBlend = 1.f - (1.f - blend) * (1.f - blend) * (1.f - blend);
        if (mBodyGroundBlend == 0.f)
        {
            mBodyGroundNormal = mBodyGroundUp = osg::Vec3f(0.f, 0.f, 1.f);
            mBodyGroundOffset = mBodyGroundLift = 0.f;
            mBodyGroundAnchor = osg::Vec3f(origin);
            return;
        }

        const float response = 1.f - std::exp(-std::max(duration, 0.f) / .055f);
        std::optional<osg::Vec3d> footSupport, middleSupport;
        const osg::Vec3f support = stats.mBrutalSlideGroundNormal;
        const bool groundedLow = stats.mBrutalSlideLow && support.length2() > .9f && support.z() > .65f;
        const auto* rays = MWBase::Environment::get().getWorld()->getRayCasting();
        MWPhysics::RayCastingResult center{};
        if (groundedLow)
            center = rays->castRay(osg::Vec3f(origin + osg::Vec3d(0., 0., 16.)),
                osg::Vec3f(origin - osg::Vec3d(0., 0., 48.)), MWPhysics::CollisionType_World
                    | MWPhysics::CollisionType_HeightMap | MWPhysics::CollisionType_Door);
        bool supportedLow = groundedLow && center.mHit && center.mHitNormal.z() > .694f;
        if (supportedLow && support * center.mHitNormal > .999f)
        {
            const osg::Vec3d contact(stats.mBrutalSlideGroundPoint);
            const double hullPlaneZ = contact.z()
                - (support.x() * (origin.x() - contact.x()) + support.y() * (origin.y() - contact.y())) / support.z();
            // A parallel lower floor is a step/drop, not a continuation of the
            // accepted support. Do not move the visual body onto it prematurely.
            supportedLow = std::abs(center.mHitPos.z() - hullPlaneZ) < 2.;
        }
        if (supportedLow)
        {
            osg::Vec3f normal = center.mHitNormal;
            normal.normalize();
            // The hull's rear edge can still touch the old plane after its center
            // crosses a join. Accepted hull support proves grounding; the short
            // center ray supplies the VISUAL plane, never a movement correction.
            // Recompute the relative offset exactly: filtering it independently
            // of the moving hull would bury/raise the world-space body anchor.
            const double planeZ = center.mHitPos.z();
            mBodyGroundOffset = std::clamp(static_cast<float>(planeZ + 1. - origin.z()), -48.f, 48.f);

            // The extended foot reaches a slope/flat join before the hull. Fit a
            // secant along the visible leg's reach, retaining the cross-slope grade.
            // These short read-only rays use the physics scheduler's lock; actors
            // are excluded and no movement state is read or changed here.
            osg::Vec3d reach;
            for (const auto bone : { "Bip01 L Foot", "Bip01 R Foot" })
                if (const auto foot = mBodyAwarenessAnimation->getBoneWorldMatrix(bone))
                {
                    osg::Vec3d distance = osg::Vec3d(foot->getTrans()) - origin;
                    distance.z() = 0.;
                    if (distance.length2() > reach.length2())
                        reach = distance;
                }
            if (reach.length2() > 20. * 20.)
            {
                reach *= std::min(reach.length() + 12., 140.) / reach.length(); // Include the toe beyond the ankle.
                const osg::Vec3d grade(normal.x() / normal.z(), normal.y() / normal.z(), 0.);
                const auto floorAt = [&](double fraction) {
                    const osg::Vec3d xy = origin + reach * fraction;
                    const double z = planeZ - grade * reach * fraction;
                    return rays->castRay(osg::Vec3f(xy.x(), xy.y(), z + 48.),
                        osg::Vec3f(xy.x(), xy.y(), z - 48.), MWPhysics::CollisionType_World
                            | MWPhysics::CollisionType_HeightMap | MWPhysics::CollisionType_Door);
                };
                const auto foot = floorAt(1.);
                if (foot.mHit && foot.mHitNormal.z() > .694f)
                {
                    const double difference = foot.mHitPos.z() - (planeZ - grade * reach);
                    if (std::abs(difference) < .5)
                        footSupport = osg::Vec3d(foot.mHitPos);
                    else
                    {
                        const osg::Vec3d otherGrade(foot.mHitNormal.x() / foot.mHitNormal.z(),
                            foot.mHitNormal.y() / foot.mHitNormal.z(), 0.);
                        const double denominator = (grade - otherGrade) * reach;
                        const double join = std::abs(denominator) > .01
                            ? (planeZ - foot.mHitPos.z() - otherGrade * reach) / denominator : -1.;
                        // Parallel surfaces at different heights are a step/drop,
                        // not a ramp. A real join must lie between the supports,
                        // with a walkable midpoint on one of the measured planes.
                        if (join >= 0. && join <= 1.)
                        {
                            const auto middle = floorAt(.5);
                            const double centerPlane = planeZ - grade * reach * .5;
                            const double footPlane = foot.mHitPos.z() + otherGrade * reach * .5;
                            if (middle.mHit && middle.mHitNormal.z() > .694f
                                && std::min(std::abs(middle.mHitPos.z() - centerPlane),
                                       std::abs(middle.mHitPos.z() - footPlane)) < 2.)
                            {
                                const osg::Vec3d secant = grade - reach * (difference / reach.length2());
                                osg::Vec3f target(secant.x(), secant.y(), 1.f);
                                target.normalize();
                                if (target.z() > .694f)
                                {
                                    normal = target;
                                    footSupport = osg::Vec3d(foot.mHitPos);
                                    middleSupport = osg::Vec3d(middle.mHitPos);
                                }
                            }
                        }
                    }
                }
            }
            mBodyGroundNormal += (normal - mBodyGroundNormal) * response;
            mBodyGroundNormal.normalize();
            for (int sampleIndex = 0; sampleIndex < 2; ++sampleIndex)
            {
                const auto& sample = sampleIndex == 0 ? footSupport : middleSupport;
                if (!sample || mBodyGroundBlend <= .999f)
                    continue;
                // Bound filter lag at a concave join. At most six units of visual
                // lift remain; larger clearance needs advance the secant tilt.
                // Bound excess toe height too when descending off a convex crest.
                // The midpoint protects knees but may sit below a bridging body.
                osg::Vec3d span = *sample - origin;
                span.z() = 0.;
                osg::Vec3d grade(mBodyGroundNormal.x() / mBodyGroundNormal.z(),
                    mBodyGroundNormal.y() / mBodyGroundNormal.z(), 0.);
                const double deficit = sample->z() + 1.
                    - (origin.z() + mBodyGroundOffset - grade * span);
                const double bounded = sampleIndex == 0 ? std::clamp(deficit, -6., 6.) : std::min(deficit, 6.);
                if (deficit != bounded)
                {
                    grade -= span * ((deficit - bounded) / span.length2());
                    mBodyGroundNormal = osg::Vec3f(grade.x(), grade.y(), 1.f);
                    mBodyGroundNormal.normalize();
                }
            }
        }
        // When support is missing, keep only the relative tilt and offset. OUT
        // still follows fresh support; an airborne body never follows an old plane.
        osg::Quat tilt;
        tilt.makeRotate(osg::Vec3f(0.f, 0.f, 1.f), mBodyGroundNormal);
        osg::Quat rotation;
        rotation.slerp(mBodyGroundBlend, osg::Quat(), tilt);
        mBodyGroundUp = rotation * osg::Vec3f(0.f, 0.f, 1.f);
        float lift = 0.f;
        for (const auto& sample : { footSupport, middleSupport })
        {
            if (!sample || mBodyGroundBlend <= .999f)
                continue;
            osg::Vec3d reach = *sample - origin;
            reach.z() = 0.;
            const osg::Vec3d grade(mBodyGroundUp.x() / mBodyGroundUp.z(), mBodyGroundUp.y() / mBodyGroundUp.z(), 0.);
            lift = std::max(lift, std::clamp(static_cast<float>(sample->z() + 1.
                - (origin.z() + mBodyGroundOffset * mBodyGroundBlend - grade * reach)), 0.f, 6.f));
        }
        if (supportedLow)
            mBodyGroundLift = lift > mBodyGroundLift ? lift : mBodyGroundLift + (lift - mBodyGroundLift) * response;
        const osg::Vec3d anchor = origin
            + osg::Vec3d(0., 0., (mBodyGroundOffset + mBodyGroundLift) * mBodyGroundBlend);
        mBodyGroundAnchor = osg::Vec3f(anchor);
        const osg::Matrixd delta = osg::Matrixd::translate(-origin) * osg::Matrixd::rotate(rotation)
            * osg::Matrixd::translate(anchor);
        osg::Matrixd inverse;
        if (inverse.invert(parentWorld))
            // Conjugate the WORLD rotation through actor yaw/race scale. Rotating
            // in local space before nonuniform scale would distort the pose and
            // fail to make its plane parallel when the player looks sideways.
            mBodyAwarenessRoot->setMatrix(mount * parentWorld * delta * inverse);
    }

    void NpcAnimation::updateBodyAwarenessClinchVisibility()
    {
        mBodyAwarenessHiddenForClinch = false;
        if (!mBodyAwarenessRoot || mBodyAwareness || mViewMode != VM_FirstPerson || mPtr.isEmpty())
            return;
        const auto& stats = mPtr.getClass().getCreatureStats(mPtr);
        // DM supplies an independent first-person arms/camera performance, not
        // a matching full-body versus clip. Keep this secondary body out of the
        // view until the normal pose transition has finished. Cull-only gating
        // preserves all visibility masks and continues updating the hidden pose.
        mBodyAwarenessHiddenForClinch = stats.mBrutalClinchActive || stats.mBrutalClinchBlendOut > 0.f;
    }

    bool NpcAnimation::usePrimaryCrouchTransitions() const
    {
        return !mBodyAwareness && mViewMode == VM_FirstPerson && !useFullBodyFirstPerson()
            && mPtr == MWMechanics::getPlayer() && MWMechanics::brutalDmFirstPerson();
    }

    bool NpcAnimation::useBodyCrouchTransitions() const
    {
        return mBodyAwareness && mPtr == MWMechanics::getPlayer() && MWMechanics::brutalDmFirstPerson()
            && MWBase::Environment::get().getWorld()->isFirstPerson();
    }

    void NpcAnimation::syncBodyAwareness(float duration)
    {
        updateBodyAwarenessClinchVisibility();
        if (!mBodyAwarenessAnimation)
            return;

        alignBodyAwareness(duration);
        NpcAnimation& body = *mBodyAwarenessAnimation;
        if (body.mAlpha != mAlpha)
            body.setAlpha(mAlpha);
        body.mLegsYawRadians = mLegsYawRadians;
        body.mUpperBodyYawRadians = mUpperBodyYawRadians;
        body.mBodyPitchRadians = mBodyPitchRadians;

        bool reset = false;
        std::vector<std::string> obsolete;
        for (const auto& [group, state] : body.mStates)
            if (!mStates.contains(group))
                obsolete.push_back(group);
        for (const std::string& group : obsolete)
        {
            body.mStates.erase(group);
            reset = true;
        }
        // A play() call normally rewires controllers immediately. Mirroring idle
        // and movement in the same frame must not expose an intermediate idle to
        // blend-rule history: only the final priority winners become visible.
        body.mDeferActiveGroupReset = true;
        try
        {
            for (const auto& [group, source] : mStates)
            {
                if (!body.hasAnimation(group))
                    continue;
                auto found = body.mStates.find(group);
                if (found == body.mStates.end() || found->second.mStartKey != source.mStartKey
                    || found->second.mStopKey != source.mStopKey)
                {
                    // Secondary visuals must never send animationEnded events for the player.
                    // play() removes equal-priority states with notifications; remove those
                    // locally before invoking it, and leave the real controller authoritative.
                    for (auto it = body.mStates.begin(); it != body.mStates.end();)
                        if (it->first == group || it->second.mPriority == source.mPriority)
                            it = body.mStates.erase(it);
                        else
                            ++it;
                    body.play(group, source.mPriority, source.mBlendMask, false, 0.f,
                        source.mStartKey, source.mStopKey, 0.f, 0, false);
                    found = body.mStates.find(group);
                    if (found == body.mStates.end())
                        continue;
                }
                AnimState& target = found->second;
                if (!(target.mPriority == source.mPriority) || target.mBlendMask != source.mBlendMask)
                {
                    target.mPriority = source.mPriority;
                    target.mBlendMask = source.mBlendMask;
                    reset = true;
                }
                // Body and arms use different rigs and clip lengths. Map each marker segment of the
                // controller (lead-in, loop, lead-out) onto the same segment of the body clip. Loop
                // markers come from the clips' text keys: the body never plays, so it never reaches
                // its own loop stop at run time.
                const auto [sourceLoopStart, sourceLoopStop] = getLoopRange(source);
                const auto [targetLoopStart, targetLoopStop] = body.getLoopRange(target);
                const auto mapSegment = [](float time, float from0, float from1, float to0, float to1) {
                    return from1 > from0 ? to0 + std::clamp((time - from0) / (from1 - from0), 0.f, 1.f) * (to1 - to0)
                                         : to1;
                };
                const float time = source.getTime();
                if (time < sourceLoopStart)
                    target.setTime(mapSegment(time, source.mStartTime, sourceLoopStart, target.mStartTime, targetLoopStart));
                else if (time <= sourceLoopStop)
                {
                    // A controller loop of another length (stock arms over imported legs) must not
                    // stretch the body cycle: the body loop then keeps its own authored rate.
                    const float targetLoopLength = targetLoopStop - targetLoopStart;
                    const float current = target.getTime();
                    if (targetLoopLength > 0.f && std::abs((sourceLoopStop - sourceLoopStart) - targetLoopLength) > 0.001f
                        && current >= targetLoopStart && current <= targetLoopStop)
                    {
                        float next = current + std::max(duration, 0.f);
                        if (next > targetLoopStop)
                            next = targetLoopStart + std::fmod(next - targetLoopStart, targetLoopLength);
                        target.setTime(next);
                    }
                    else
                        target.setTime(mapSegment(time, sourceLoopStart, sourceLoopStop, targetLoopStart, targetLoopStop));
                }
                else
                    target.setTime(mapSegment(time, sourceLoopStop, source.mStopTime, targetLoopStop, target.mStopTime));
                target.mSpeedMult = 0.f;
                target.mLoopCount = 0;
                target.mPlaying = false;
                target.mAutoDisable = false;
            }
        }
        catch (...)
        {
            body.mDeferActiveGroupReset = false;
            throw;
        }
        body.mDeferActiveGroupReset = false;
        if (reset || body.mDeferredActiveGroupReset)
            body.resetActiveGroups();
        body.runAnimation(0.f); // Updates pose controllers only; no text-key listener or root motion consumer.
    }

    void NpcAnimation::setFirstPersonGeometryVisibleForAudit(bool visible)
    {
        const unsigned int mask = mViewMode == VM_FirstPerson && !useFullBodyFirstPerson()
            ? Mask_FirstPerson : ~0u;
        mObjectRoot->setNodeMask(visible ? mask : 0u);
        if (mBodyAwarenessRoot)
            mBodyAwarenessRoot->setNodeMask(visible ? Mask_Player : 0u);
    }

    std::string NpcAnimation::getSheathedShieldMesh(const MWWorld::ConstPtr& shield) const
    {
        std::string mesh = getShieldMesh(shield, !mNpc->isMale());

        if (mesh.empty())
            return std::string();

        const VFS::Path::Normalized holsteredName(addSuffixBeforeExtension(mesh, "_sh"));
        if (mResourceSystem->getVFS()->exists(holsteredName))
        {
            osg::ref_ptr<osg::Node> shieldTemplate = mResourceSystem->getSceneManager()->getInstance(holsteredName);
            SceneUtil::FindByNameVisitor findVisitor("Bip01 Sheath");
            shieldTemplate->accept(findVisitor);
            osg::ref_ptr<osg::Node> sheathNode = findVisitor.mFoundNode;
            if (!sheathNode)
                return std::string();
        }

        return mesh;
    }

    VFS::Path::Normalized NpcAnimation::resolvePlayerEquipmentMesh(VFS::Path::NormalizedView normalMesh) const
    {
        if (!Settings::game().mCurvyBodyMeshes || mPtr != MWMechanics::getPlayer() || mNpc->isMale())
            return VFS::Path::Normalized(normalMesh);

        constexpr std::string_view meshesPrefix = "meshes/";
        constexpr std::string_view curvyPrefix = "meshes/curvybody/";
        const std::string_view path = normalMesh.value();
        if (!path.starts_with(meshesPrefix) || path.starts_with(curvyPrefix))
            return VFS::Path::Normalized(normalMesh);

        VFS::Path::Normalized candidate(std::string(curvyPrefix) + std::string(path.substr(meshesPrefix.size())));
        return mResourceSystem->getVFS()->exists(candidate) ? candidate : VFS::Path::Normalized(normalMesh);
    }

    VFS::Path::Normalized NpcAnimation::resolvePlayerNakedBodyMesh() const
    {
        if (!Settings::game().mCurvyNakedBody || mPtr != MWMechanics::getPlayer() || mNpc->isMale()
            || mViewMode == VM_FirstPerson || getNpcType() == Type_Werewolf)
            return {};

        static const std::pair<ESM::RefId, std::string_view> bodyMeshes[] = {
            { ESM::RefId::stringRefId("breton"), "meshes/bbr/br_f.nif" },
            { ESM::RefId::stringRefId("dark elf"), "meshes/bbr/de_f.nif" },
            { ESM::RefId::stringRefId("high elf"), "meshes/bbr/he_f.nif" },
            { ESM::RefId::stringRefId("imperial"), "meshes/bbr/im_f.nif" },
            { ESM::RefId::stringRefId("nord"), "meshes/bbr/no_f.nif" },
            { ESM::RefId::stringRefId("orc"), "meshes/bbr/or_f.nif" },
            { ESM::RefId::stringRefId("redguard"), "meshes/bbr/rg_f.nif" },
            { ESM::RefId::stringRefId("wood elf"), "meshes/bbr/we_f.nif" },
        };

        for (const auto& [race, path] : bodyMeshes)
        {
            if (mNpc->mRace == race)
            {
                VFS::Path::Normalized candidate(path);
                return mResourceSystem->getVFS()->exists(candidate) ? candidate : VFS::Path::Normalized();
            }
        }
        return {};
    }

    NpcAnimation::BrutalViewmodel NpcAnimation::brutalViewmodelSet() const
    {
        if (!mPtr.getClass().hasInventoryStore(mPtr))
            return BrutalViewmodel_Default;
        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        const auto weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        const int type = weapon != inv.end() && weapon->getType() == ESM::Weapon::sRecordId
            ? weapon->get<ESM::Weapon>()->mBase->mData.mType : ESM::Weapon::None;
        // OpenMW keeps a shield equipped while a two-handed weapon hides it. Its
        // inventory slot must not override the weapon's animation set.
        if (MWMechanics::brutalStaffWeapon(type))
            return BrutalViewmodel_Staff;
        if (MWMechanics::getWeaponType(type)->mFlags & ESM::WeaponType::TwoHanded)
            return BrutalViewmodel_Default;
        if (carriesShield())
            return BrutalViewmodel_Shield;
        // The paired source rig is meaningful only with two actual short
        // blades. A single blade uses the sword palm/mount and animation set;
        // the shield case above already selects the sword-and-shield set.
        const auto left = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
        const bool twoDaggers = MWMechanics::brutalDualWield() && type == ESM::Weapon::ShortBladeOneHand && left != inv.end()
            && left->getType() == ESM::Weapon::sRecordId
            && left->get<ESM::Weapon>()->mBase->mData.mType == ESM::Weapon::ShortBladeOneHand;
        return twoDaggers ? BrutalViewmodel_Dagger : BrutalViewmodel_Default;
    }

    bool NpcAnimation::useBrutalStaffSet() const
    {
        return mViewMode == VM_FirstPerson && !mBodyAwareness && mViewmodelSet == BrutalViewmodel_Staff
            && getNode(sBrutalLeftWeaponBone) != nullptr;
    }

    bool NpcAnimation::carriesLeftWeapon() const
    {
        if (!MWMechanics::brutalDualWield() || !mPtr.getClass().hasInventoryStore(mPtr))
            return false;
        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        const auto left = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
        return left != inv.end() && left->getType() == ESM::Weapon::sRecordId;
    }

    void NpcAnimation::reloadBrutalAnimations()
    {
        rebuild();
        if (MWRender::Camera* camera = MWBase::Environment::get().getWorld()->getCamera();
            camera && camera->getTrackingPtr() == mPtr)
            camera->setAnimation(this);
        MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(mPtr);
    }

    bool NpcAnimation::carriesShield() const
    {
        if (!mPtr.getClass().hasInventoryStore(mPtr))
            return false;
        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        const auto shield = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
        return shield != inv.end() && shield->getType() == ESM::Armor::sRecordId;
    }

    void NpcAnimation::updateParts()
    {
        clearRagdollGarmentCoverage();
        if (!mObjectRoot.get())
            return;

        NpcType curType = getNpcType();
        if (curType != mNpcType)
        {
            mNpcType = curType;
            rebuild();
            return;
        }

        if (mViewMode == VM_FirstPerson && !useFullBodyFirstPerson() && mPtr == MWMechanics::getPlayer()
            && MWMechanics::brutalDmFirstPerson() && brutalViewmodelSet() != mViewmodelSet)
        {
            rebuild(); // Reloads the first-person sources with the matching equipment set.
            // The old skeleton's Camera node is gone: let the camera resolve the new one.
            if (MWRender::Camera* camera = MWBase::Environment::get().getWorld()->getCamera();
                camera && camera->getTrackingPtr() == mPtr)
                camera->setAnimation(this);
            MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(mPtr);
            return;
        }

        static const struct
        {
            int mSlot;
            int mBasePriority;
        } slotlist[] = { // FIXME: Priority is based on the number of reserved slots. There should be a better way.
            { MWWorld::InventoryStore::Slot_Robe, 11 }, { MWWorld::InventoryStore::Slot_Skirt, 3 },
            { MWWorld::InventoryStore::Slot_Helmet, 0 }, { MWWorld::InventoryStore::Slot_Cuirass, 0 },
            { MWWorld::InventoryStore::Slot_Greaves, 0 }, { MWWorld::InventoryStore::Slot_LeftPauldron, 0 },
            { MWWorld::InventoryStore::Slot_RightPauldron, 0 }, { MWWorld::InventoryStore::Slot_Boots, 0 },
            { MWWorld::InventoryStore::Slot_LeftGauntlet, 0 }, { MWWorld::InventoryStore::Slot_RightGauntlet, 0 },
            { MWWorld::InventoryStore::Slot_Shirt, 0 }, { MWWorld::InventoryStore::Slot_Pants, 0 },
            { MWWorld::InventoryStore::Slot_CarriedLeft, 0 }, { MWWorld::InventoryStore::Slot_CarriedRight, 0 }
        };
        static const size_t slotlistsize = sizeof(slotlist) / sizeof(slotlist[0]);

        bool wasArrowAttached = isArrowAttached();
        mAmmunition.reset();

        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        for (size_t i = 0; i < slotlistsize && mViewMode != VM_HeadOnly; i++)
        {
            MWWorld::ConstContainerStoreIterator store = inv.getSlot(slotlist[i].mSlot);

            removePartGroup(slotlist[i].mSlot);

            if (store == inv.end())
                continue;

            if (slotlist[i].mSlot == MWWorld::InventoryStore::Slot_Helmet)
                removeIndividualPart(ESM::PRT_Hair);

            int prio = 1;
            bool enchantedGlow = !store->getClass().getEnchantment(*store).empty();
            osg::Vec4f glowColor = store->getClass().getEnchantmentColor(*store);
            if (store->getType() == ESM::Clothing::sRecordId)
            {
                prio = ((slotlist[i].mBasePriority + 1) << 1) + 0;
                const ESM::Clothing* clothes = store->get<ESM::Clothing>()->mBase;
                addPartGroup(slotlist[i].mSlot, prio, clothes->mParts.mParts, enchantedGlow, &glowColor);
            }
            else if (store->getType() == ESM::Armor::sRecordId)
            {
                prio = ((slotlist[i].mBasePriority + 1) << 1) + 1;
                const ESM::Armor* armor = store->get<ESM::Armor>()->mBase;
                addPartGroup(slotlist[i].mSlot, prio, armor->mParts.mParts, enchantedGlow, &glowColor);
            }

            if (slotlist[i].mSlot == MWWorld::InventoryStore::Slot_Robe)
            {
                ESM::PartReferenceType parts[] = { ESM::PRT_Groin, ESM::PRT_Skirt, ESM::PRT_RLeg, ESM::PRT_LLeg,
                    ESM::PRT_RUpperarm, ESM::PRT_LUpperarm, ESM::PRT_RKnee, ESM::PRT_LKnee, ESM::PRT_RForearm,
                    ESM::PRT_LForearm, ESM::PRT_Cuirass };
                const size_t partsSize = sizeof(parts) / sizeof(parts[0]);
                for (size_t p = 0; p < partsSize; ++p)
                    reserveIndividualPart(parts[p], slotlist[i].mSlot, prio);
            }
            else if (slotlist[i].mSlot == MWWorld::InventoryStore::Slot_Skirt)
            {
                reserveIndividualPart(ESM::PRT_Groin, slotlist[i].mSlot, prio);
                reserveIndividualPart(ESM::PRT_RLeg, slotlist[i].mSlot, prio);
                reserveIndividualPart(ESM::PRT_LLeg, slotlist[i].mSlot, prio);
            }
        }

        if (mViewMode != VM_FirstPerson)
        {
            if (mPartPriorities[ESM::PRT_Head] < 1 && !mHeadModel.empty())
                addOrReplaceIndividualPart(ESM::PRT_Head, -1, 1, mHeadModel);
            if (mPartPriorities[ESM::PRT_Hair] < 1 && mPartPriorities[ESM::PRT_Head] <= 1 && !mHairModel.empty())
                addOrReplaceIndividualPart(ESM::PRT_Hair, -1, 1, mHairModel);
        }
        if (mViewMode == VM_HeadOnly)
            return;

        if (mPartPriorities[ESM::PRT_Shield] < 1)
        {
            MWWorld::ConstContainerStoreIterator store = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
            MWWorld::ConstPtr part;
            if (store != inv.end() && (part = *store).getType() == ESM::Light::sRecordId)
            {
                const ESM::Light* light = part.get<ESM::Light>()->mBase;
                addOrReplaceIndividualPart(ESM::PRT_Shield, MWWorld::InventoryStore::Slot_CarriedLeft, 1,
                    Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(light->mModel)), false, nullptr, true);
                if (mObjectParts[ESM::PRT_Shield])
                    addExtraLight(mObjectParts[ESM::PRT_Shield]->getNode()->asGroup(), SceneUtil::LightCommon(*light));
            }
        }

        showWeapons(mShowWeapons);
        showCarriedLeft(mShowCarriedLeft);

        bool isWerewolf = (getNpcType() == Type_Werewolf);
        // displayBodyRace() returns the Hazaeki transformation's target race when a humanoid form is
        // active, so the naked skin is the form's race while equipment stays the player's own.
        ESM::RefId race = (isWerewolf ? ESM::RefId::stringRefId("werewolf") : displayBodyRace());

        // The Mang'Chu form has no race, so its skin is resolved by explicit part ids into the same
        // PRT_-indexed layout; every other case uses the (cached) race lookup.
        std::vector<const ESM::BodyPart*> mangchuParts;
        if (mHazMangchu && !isWerewolf)
            mangchuParts = buildMangchuBodyParts(!mNpc->isMale());
        const std::vector<const ESM::BodyPart*>& parts = (mHazMangchu && !isWerewolf)
            ? mangchuParts
            : getBodyParts(race, !mNpc->isMale(), mViewMode == VM_FirstPerson && !useFullBodyFirstPerson(), isWerewolf);

        // BBR meshes (Curvybody for Morrowind) follow the same multi-geometry convention as Better Bodies:
        // one NIF contains "Tri Neck", "Tri Chest", "Tri Groin", etc. geometry nodes. OpenMW's
        // SceneUtil::attach applies the slot's bone-name as a filter (e.g. "Chest" for PRT_Cuirass),
        // so attaching the BBR mesh to EACH slot individually renders only that slot's geometry.
        // Only activate this path when the player is actually naked (no equipment owns any body-part slot),
        // so the curvy body cannot overdraw equipped armor or clothing.
        // Use mPartslots[part] >= 0 (equipment-owned) not mPartPriorities >= 1, because body-part meshes
        // attached with slot=-1 also carry priority 1 and would falsely fire the check every subsequent call.

        // When the BBR naked-body feature can activate, clear stale body-part-owned (slot=-1) meshes
        // from earlier calls so the loop below can re-evaluate BBR vs. BB on every updateParts() call.
        // Without this, switching from dressed to naked (or vice versa) would leave the wrong mesh in
        // slots whose priority is already 1 — preventing the desired mesh from loading there.
        if (Settings::game().mCurvyNakedBody && mPtr == MWMechanics::getPlayer() && !mNpc->isMale())
        {
            for (int part = ESM::PRT_Neck; part < ESM::PRT_Count; ++part)
                if (mPartslots[part] == -1 && mPartPriorities[part] > 0)
                    removeIndividualPart(static_cast<ESM::PartReferenceType>(part));
        }

        bool hasCoveredBodyPart = false;
        for (int part = ESM::PRT_Neck; part < ESM::PRT_Count; ++part)
            hasCoveredBodyPart = hasCoveredBodyPart || (parts[part] && mPartslots[part] >= 0);
        // Normally the curvy naked (BBR) body is only used when the player is fully naked, reverting
        // exposed skin to Better Bodies as soon as anything is equipped. But when the equipment-mesh
        // override ("curvy body meshes") is also on, the player is wearing curvy equipment, so the
        // exposed skin around it should stay curvy too - use the BBR mesh for the exposed slots
        // instead of Better Bodies. The loop below only fills slots equipment doesn't own
        // (priority < 1), and the per-slot bone filter renders only that slot's region, so equipped
        // armor/clothing is never overdrawn.
        const bool curvyUnderEquipment = Settings::game().mCurvyBodyMeshes && Settings::game().mCurvyNakedBody;
        const VFS::Path::Normalized nakedBodyMesh
            = (hasCoveredBodyPart && !curvyUnderEquipment) ? VFS::Path::Normalized() : resolvePlayerNakedBodyMesh();

        const bool fittedRobe = mPtr == MWMechanics::getPlayer() && mViewMode == VM_FirstPerson
            && !useFullBodyFirstPerson() && !mBodyAwareness
            && mObjectParts[ESM::PRT_Cuirass] && mPartslots[ESM::PRT_Cuirass] == MWWorld::InventoryStore::Slot_Robe;
        for (const auto [wrist, forearm] : { std::pair{ESM::PRT_LWrist, ESM::PRT_LForearm},
                 std::pair{ESM::PRT_RWrist, ESM::PRT_RForearm} })
        {
            const std::string wanted = fittedRobe && mPartslots[forearm] == MWWorld::InventoryStore::Slot_Robe
                ? mPartModels[ESM::PRT_Cuirass] : std::string{};
            if (mPartslots[wrist] == -1 && mFirstPersonWristCoverage[wrist] != wanted)
                removeIndividualPart(wrist);
        }
        for (int part = ESM::PRT_Neck; part < ESM::PRT_Count; ++part)
        {
            if (mPartPriorities[part] < 1)
            {
                if (const ESM::BodyPart* bodypart = parts[part])
                {
                    // When the BBR naked-body mesh is active, use it for every slot that has a BB
                    // body part. The per-slot bone filter extracts the correct geometry region from
                    // the BBR NIF (same "Tri X" naming convention used by Better Bodies).
                    const VFS::Path::Normalized mesh = !nakedBodyMesh.empty()
                        ? nakedBodyMesh
                        : Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(bodypart->mModel));
                    addOrReplaceIndividualPart(static_cast<ESM::PartReferenceType>(part), -1, 1, mesh);
                }
            }
        }

        if (fittedRobe)
            fitFirstPersonWrists();

        if (wasArrowAttached)
            attachArrow();

        // Unequipped parts may have left their Verlet cloth chains in the skeleton; drop unused ones.
        pruneUnusedEquipmentBones();

        // Run model-specific secondary-motion compatibility for every actor after equipment
        // attachment. Female body auto-rigging is still explicitly gated by sex.
        const std::string actorDisplayName = std::string(mPtr.getClass().getName(mPtr));
        JiggleAutoRig::run(mObjectRoot.get(), mPtr == MWMechanics::getPlayer(), !mNpc->isMale(), actorDisplayName,
            mPtr.getCellRef().getRefNum());
        if (mBodyAwarenessAnimation)
            mBodyAwarenessAnimation->updateParts();
        prewarmDecapitationMaterial();
        prewarmBloodDecals();
        rebuildBloodDecals();
        if (!mDecapitation && !mBisection && !mLimbSevering)
        {
            // Compile the actual split exterior/cap layouts and material paths during
            // scene/equipment setup; preparation performs no visible scene mutation.
            const int failure=mDecapitationFailure;
            rebuildSeveredHead(true);
            mDecapitationFailure=failure;
        }
        if (mDecapitation && !rebuildSeveredHead())
        {
            // Keep the previous closed fragment rather than expose a rebuilt intact head.
            for (auto type : {ESM::PRT_Head, ESM::PRT_Hair, ESM::PRT_Neck})
                if (mObjectParts[type])
                {
                    auto node=mObjectParts[type]->getNode();
                    const bool recorded=std::any_of(mDecapitation->originals.begin(),mDecapitation->originals.end(),
                        [&](const auto& entry){return entry.first==node;});
                    if(!recorded)mDecapitation->originals.emplace_back(node,node->getNodeMask());
                    node->setNodeMask(0);
                }
            Log(Debug::Warning) << "Decapitation equipment rebuild rejected unsupported geometry";
        }
        if (mBisection && !rebuildSeveredTorso())
        {
            // Keep the last complete pair if a newly equipped custom mesh cannot
            // produce a closed cut. Never expose an intact bridge between halves.
            for (auto part : { ESM::PRT_Cuirass, ESM::PRT_Groin, ESM::PRT_Skirt })
                if (mObjectParts[part])
                {
                    auto node = mObjectParts[part]->getNode();
                    const bool recorded = std::any_of(mBisection->originals.begin(), mBisection->originals.end(),
                        [&](const auto& entry) { return entry.first == node; });
                    if (!recorded) mBisection->originals.emplace_back(node, node->getNodeMask());
                    node->setNodeMask(0);
                }
            Log(Debug::Warning) << "Bisection equipment rebuild rejected unsupported geometry";
        }
        if (mLimbSevering && !rebuildSeveredLimbs(mLimbSevering->mask))
        {
            for (int part = 0; part < ESM::PRT_Count; ++part)
                if (mLimbReplacedParts[part] && mObjectParts[part])
                {
                    auto node = mObjectParts[part]->getNode();
                    const bool recorded = std::any_of(mLimbSevering->originals.begin(), mLimbSevering->originals.end(),
                        [&](const auto& entry) { return entry.first == node; });
                    if (!recorded) mLimbSevering->originals.emplace_back(node, node->getNodeMask());
                    node->setNodeMask(0);
                }
            Log(Debug::Warning) << "Limb equipment rebuild retained previous closed geometry";
        }
    }

    PartHolderPtr NpcAnimation::insertBoundedPart(VFS::Path::NormalizedView model, std::string_view bonename,
        std::string_view bonefilter, bool enchantedGlow, osg::Vec4f* glowColor, bool isLight)
    {
        osg::ref_ptr<osg::Node> attached = attach(model, bonename, bonefilter, isLight);
        if (enchantedGlow)
            mGlowUpdater = SceneUtil::addEnchantedGlow(attached, mResourceSystem, *glowColor);

        // Multi-piece equipment: "AttachTo:<bone>" nodes move onto their own
        // bones. The glow above is applied first so every piece keeps it.
        const NodeMap& nodeMap = getNodeMap();
        const SceneUtil::BonePieceResult pieces
            = SceneUtil::relocateBonePieces(*attached, [&nodeMap](std::string_view bone) -> osg::Group* {
                  const auto found = nodeMap.find(bone);
                  return found == nodeMap.end() ? nullptr : found->second.get();
              });
        for (const std::string& missing : pieces.mMissingBones)
            Log(Debug::Warning) << "Part \"" << model.value() << "\": no bone \"" << missing
                                << "\" for an AttachTo piece; it stays on " << bonename;

        if (!pieces.mRelocated.empty())
            Log(Debug::Verbose) << "Part \"" << model.value() << "\": moved " << pieces.mRelocated.size()
                                << " AttachTo piece(s) onto their bones";

        auto holder = std::make_unique<PartHolder>(std::move(attached));
        for (const osg::ref_ptr<osg::Node>& piece : pieces.mRelocated)
            holder->addLinkedNode(piece);
        return holder;
    }

    osg::Vec3f NpcAnimation::runAnimation(float timepassed)
    {
        // Apply the Hazaeki transformation body override before running the frame. Cheap: it only
        // reads two globals and rebuilds when the form code actually changes.
        updateBodyRaceOverride();

        restoreTransientPose();
        prewarmBloodDecals();
        updateCutBloodFlow(timepassed);
        if(mDecapitationRenderPrewarm)
        {
            mDecapitationRenderPrewarm->update();
            if(mDecapitationRenderPrewarm->drawn())
            {++mDecapitationWarmDrawn;mDecapitationRenderPrewarm.reset();}
            else if(mDecapitationRenderPrewarm->timedOut())
            {++mDecapitationWarmTimedOut;mDecapitationRenderPrewarm.reset();}
        }
        if(mDecapitation && (!Settings::game().mBrutalDismemberment || !Settings::game().mBrutalRagdoll || !mPtr.getClass().getNpcStats(mPtr).isDead()))
            setHeadSevered(false);
        if(mBisection && (!Settings::game().mBrutalDismemberment || !Settings::game().mBrutalRagdoll || !mPtr.getClass().getNpcStats(mPtr).isDead()))
            setTorsoSevered(false);
        if(mLimbSevering && (!Settings::game().mBrutalDismemberment || !Settings::game().mBrutalRagdoll || !mPtr.getClass().getNpcStats(mPtr).isDead()))
            setLimbsSevered(0);
        osg::Vec3f ret = Animation::runAnimation(timepassed);

        mHeadAnimationTime->update(timepassed);

        if (mFirstPersonNeckController)
        {
            if (mAccurateAiming)
                mAimingFactor = 1.f;
            else
                mAimingFactor = std::max(0.f, mAimingFactor - timepassed * 0.5f);

            float rotateFactor = useFullBodyFirstPerson() || MWMechanics::brutalDmFirstPerson()
                ? 1.f
                : 0.75f + 0.25f * mAimingFactor;

            mFirstPersonNeckController->setRotate(
                osg::Quat(mPtr.getRefData().getPosition().rot[0] * rotateFactor, osg::Vec3f(-1, 0, 0)));
            mFirstPersonNeckController->setOffset(useFullBodyFirstPerson() ? osg::Vec3f() : mFirstPersonOffset);
            if (useFullBodyFirstPerson())
                mFirstPersonNeckController->setRotationPivot(
                    osg::Vec3f(0.f, Settings::game().mFirstPersonBodyForward.get(), 124.f + mFirstPersonOffset.z()));
            else if (MWMechanics::brutalDmFirstPerson())
            {
                // The imported rig supplies its source-derived base eye. CameraAdd
                // is an absolute offset about this pivot, including during pitch.
                if (const auto* pivot = dynamic_cast<const osg::MatrixTransform*>(getNode("Brutal Camera Pivot")))
                    mFirstPersonNeckController->setRotationPivot(pivot->getMatrix().getTrans());
            }
        }

        if (!mBodyAwareness)
            WeaponAnimation::configureControllers(mPtr.getRefData().getPosition().rot[0] + getBodyPitchRadians());
        syncBodyAwareness(timepassed);

        updateRagdollGarmentCoverage(timepassed);
        updateRagdollGarmentAudit(timepassed);

        return ret;
    }

    int NpcAnimation::getPartForNodePath(const osg::NodePath& path) const
    {
        for(int part=0;part<ESM::PRT_Count;++part)
            if(mObjectParts[part] && std::find(path.begin(),path.end(),mObjectParts[part]->getNode().get())!=path.end())
                return part;
        if(mBodyAwarenessAnimation)
            return mBodyAwarenessAnimation->getPartForNodePath(path);
        return -1;
    }

    std::map<std::string, osg::Vec3f> NpcAnimation::getFootGeometryForAudit(const osg::Vec3f& normal) const
    {
        std::map<std::string, osg::Vec3f> result;
        const std::function<std::optional<osg::Matrixf>(std::string_view)> bones =
            [&](std::string_view name) { return getBoneWorldMatrix(name); };
        for (const auto& [part, prefix] : { std::pair{ ESM::PRT_LFoot, "leftFootMesh" },
                 std::pair{ ESM::PRT_RFoot, "rightFootMesh" } })
        {
            if (!mObjectParts[part])
                continue;
            // Snapshot the current pose, not a previous cull's bounding box. This
            // path is audit-only and returns extrema from actual worn/bare vertices.
            std::vector<DecapitationGeometry::Snapshot> snapshots;
            DecapitationGeometry::SnapshotVisitor visitor(snapshots, bones, osg::Matrixf(), true);
            visitor.snapshot(mObjectParts[part]->getNode());
            if (visitor.failed)
                continue;
            float low = std::numeric_limits<float>::max(), high = -low;
            osg::Vec3f lowPoint, highPoint;
            for (const auto& snapshot : snapshots)
                for (const auto& point : *static_cast<const osg::Vec3Array*>(snapshot.geometry->getVertexArray()))
                {
                    const float height = point * normal;
                    if (height < low)
                    {
                        low = height;
                        lowPoint = point;
                    }
                    if (height > high)
                    {
                        high = height;
                        highPoint = point;
                    }
                }
            if (low <= high)
            {
                result[std::string(prefix) + "Low"] = lowPoint;
                result[std::string(prefix) + "High"] = highPoint;
            }
        }
        return result;
    }

    NpcAnimation::WeaponGripGeometry NpcAnimation::getWeaponGripGeometryForAudit() const
    {
        WeaponGripGeometry result;
        const std::function<std::optional<osg::Matrixf>(std::string_view)> boneWorld
            = [&](std::string_view name) { return getBoneWorldMatrix(name); };
        for (const auto& [name, part] : { std::pair{ "leftHand", ESM::PRT_LHand },
                 std::pair{ "rightHand", ESM::PRT_RHand }, std::pair{ "leftItem", ESM::PRT_Shield },
                 std::pair{ "rightItem", ESM::PRT_Weapon },
                 std::pair{ "leftForearm", ESM::PRT_LForearm },
                 std::pair{ "rightForearm", ESM::PRT_RForearm },
                 std::pair{ "leftUpperArm", ESM::PRT_LUpperarm },
                 std::pair{ "rightUpperArm", ESM::PRT_RUpperarm },
                 std::pair{ "leftWrist", ESM::PRT_LWrist },
                 std::pair{ "rightWrist", ESM::PRT_RWrist },
                 std::pair{ "robe", ESM::PRT_Cuirass } })
        {
            if (!mObjectParts[part])
                continue;
            auto node = mObjectParts[part]->getNode();
            result.models[name] = mPartModels[part];
            const auto paths = node->getParentalNodePaths();
            if (!paths.empty())
                result.roots[name] = osg::computeLocalToWorld(paths.front());
            std::vector<DecapitationGeometry::Snapshot> snapshots;
            DecapitationGeometry::SnapshotVisitor visitor(snapshots, boneWorld, osg::Matrixf(), true);
            visitor.snapshot(node);
            if (visitor.failed)
                throw std::runtime_error("Failed to snapshot actual weapon grip geometry");
            auto& output = result.triangles[name];
            for (const auto& snapshot : snapshots)
            {
                const auto* vertices = dynamic_cast<const osg::Vec3Array*>(snapshot.geometry->getVertexArray());
                if (!vertices)
                    throw std::runtime_error("Weapon grip geometry requires Vec3 vertices");
                osg::TriangleIndexFunctor<DecapitationGeometry::TriangleList> faces;
                snapshot.geometry->accept(faces);
                for (const auto& face : faces.triangles)
                    for (const auto index : face)
                        output.push_back((*vertices)[index]);
            }
        }
        return result;
    }

    std::map<std::string, osg::Vec3f> NpcAnimation::getShieldGeometryForAudit() const
    {
        if(!mShowCarriedLeft || !mObjectParts[ESM::PRT_Shield])return {};
        std::vector<DecapitationGeometry::Snapshot> snapshots;
        const std::function<std::optional<osg::Matrixf>(std::string_view)> boneWorld=
            [&](std::string_view name){return getBoneWorldMatrix(name);};
        DecapitationGeometry::SnapshotVisitor visitor(snapshots,boneWorld,osg::Matrixf(),true);
        visitor.snapshot(mObjectParts[ESM::PRT_Shield]->getNode());
        if(visitor.failed || snapshots.empty())return {};
        osg::BoundingBox bounds;
        for(const auto& snapshot:snapshots)
            for(const auto& vertex:*static_cast<osg::Vec3Array*>(snapshot.geometry->getVertexArray()))bounds.expandBy(vertex);
        std::array<osg::Vec3f,3> chosen;float best=std::numeric_limits<float>::max();bool found=false;
        for(const auto& snapshot:snapshots)
        {
            osg::TriangleIndexFunctor<DecapitationGeometry::TriangleList> tris;snapshot.geometry->accept(tris);
            const auto* vertices=static_cast<osg::Vec3Array*>(snapshot.geometry->getVertexArray());
            for(const auto& tri:tris.triangles)
            {
                auto a=(*vertices)[tri[0]],b=(*vertices)[tri[1]],c=(*vertices)[tri[2]];
                if(((b-a)^(c-a)).length2()<.01f)continue;
                auto center=(a+b+c)/3.f;float score=(center-bounds.center()).length2();
                if(score<best){chosen={a,b,c};best=score;found=true;}
            }
        }
        if(!found)return {};
        auto normal=(chosen[1]-chosen[0])^(chosen[2]-chosen[0]);normal.normalize();
        return {{"center",(chosen[0]+chosen[1]+chosen[2])/3.f},{"normal",normal},
            {"minimum",bounds._min},{"maximum",bounds._max},
            {"triangleA",chosen[0]},{"triangleB",chosen[1]},{"triangleC",chosen[2]}};
    }

    std::shared_ptr<const ESM::ReleasedEquipmentState> NpcAnimation::captureReleasedEquipment(int slot,
        const osg::Matrixf& itemRoot, float mass, float& maxWorldVertexError) const
    {
        const bool ammunition = slot == MWWorld::InventoryStore::Slot_Ammunition;
        const bool left = slot == MWWorld::InventoryStore::Slot_CarriedLeft;
        if ((!left && !ammunition && slot != MWWorld::InventoryStore::Slot_CarriedRight)
            || !(left ? mShowCarriedLeft : mShowWeapons))
            return {};
        const auto part = left ? ESM::PRT_Shield : ESM::PRT_Weapon;
        const auto* root = ammunition ? mAmmunition.get() : mObjectParts[part].get();
        if (!root || (!ammunition && mPartModels[part].empty()))
            return {};
        auto saved = std::make_shared<ESM::ReleasedEquipmentState>();
        if (ammunition)
        {
            const auto& inventory = mPtr.getClass().getInventoryStore(mPtr);
            const auto item = inventory.getSlot(slot);
            if (item == inventory.end())
                return {};
            saved->mModel = item->getClass().getCorrectedModel(*item).value();
        }
        else
            saved->mModel = mPartModels[part];
        saved->mMass = mass;
        const std::function<std::optional<osg::Matrixd>(std::string_view)> bones =
            [&](std::string_view name) -> std::optional<osg::Matrixd>
            {
                const auto found = getNodeMap().find(std::string(name));
                if (found == getNodeMap().end()) return {};
                // Rebase before float conversion in the capture visitor. The
                // ordinary float world getter has already rounded exterior
                // coordinates and cannot recover the actual attachment pose.
                return osg::computeLocalToWorld(releasedEquipmentPath(
                    found->second, mPtr.getRefData().getBaseNode()));
            };
        ReleasedEquipmentCapture capture(*saved, root->getNode(),
            !ammunition && mAmmunition ? mAmmunition->getNode().get() : nullptr, itemRoot, bones,
            mPtr.getRefData().getBaseNode());
        capture.snapshot();
        maxWorldVertexError = capture.maxWorldVertexError;
        if (!saved->valid())
            throw std::runtime_error("Equipped item capture descriptor is invalid");
        if (!std::isfinite(maxWorldVertexError) || maxWorldVertexError > .001f)
            throw std::runtime_error("Equipped item continuity error " + std::to_string(maxWorldVertexError)
                + " exceeds 0.001 world units");
        return saved;
    }

    bool NpcAnimation::getReleasedEquipmentOwnerIsLeft(int slot) const
    {
        const PartHolder* part = slot == MWWorld::InventoryStore::Slot_Ammunition ? mAmmunition.get()
            : mObjectParts[slot == MWWorld::InventoryStore::Slot_CarriedLeft ? ESM::PRT_Shield : ESM::PRT_Weapon].get();
        if (part)
        {
            const auto path = releasedEquipmentPath(part->getNode(), mPtr.getRefData().getBaseNode());
            for (auto node = path.rbegin(); node != path.rend(); ++node)
            {
                if (Misc::StringUtils::ciEqual((*node)->getName(), "Bip01 L Forearm")
                    || Misc::StringUtils::ciEqual((*node)->getName(), "Bip01 L Hand"))
                    return true;
                if (Misc::StringUtils::ciEqual((*node)->getName(), "Bip01 R Forearm")
                    || Misc::StringUtils::ciEqual((*node)->getName(), "Bip01 R Hand"))
                    return false;
            }
        }
        if (slot == MWWorld::InventoryStore::Slot_CarriedLeft)
            return true;
        const auto& inventory = mPtr.getClass().getInventoryStore(mPtr);
        const auto weapon = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        return weapon != inventory.end() && weapon->getType() == ESM::Weapon::sRecordId
            && MWMechanics::getWeaponType(weapon->get<ESM::Weapon>()->mBase->mData.mType)->mAttachBone.find("Left")
                != std::string::npos;
    }

    Animation::RagdollExtremityVertices NpcAnimation::getRagdollExtremityVertices(
        UnsupportedRagdollExtremityBones* unsupportedBones) const
    {
        RagdollExtremityVertices result;
        if (unsupportedBones)
            unsupportedBones->clear();
        if (!isRagdollPose())
            return result;
        const std::function<std::optional<osg::Matrixf>(std::string_view)> boneWorld
            = [&](std::string_view name) { return getBoneWorldMatrix(name); };
        for (const auto& [part, bone, parent] : {
                 std::tuple{ ESM::PRT_LHand, "Bip01 L Hand", "Bip01 L Forearm" },
                 std::tuple{ ESM::PRT_RHand, "Bip01 R Hand", "Bip01 R Forearm" },
                 std::tuple{ ESM::PRT_LFoot, "Bip01 L Foot", "Bip01 L Calf" },
                 std::tuple{ ESM::PRT_RFoot, "Bip01 R Foot", "Bip01 R Calf" } })
        {
            if (!mObjectParts[part])
                continue;
            // Only weights on this physical bone or its frozen descendants can
            // remain rigidly attached to this one-body proxy throughout a fall.
            // Some files declare an UpperArm/Thigh root without weighting it.
            const auto* physicalBone = getNode(parent);
            std::set<std::string> unsupported;
            DecapitationGeometry::RetainedRigVisitor weights;
            weights.setNodeMaskOverride(~0u);
            weights.snapshot(mObjectParts[part]->getNode());
            for (const auto& [rig, path] : weights.rigs)
                for (const auto& name : rig->getInfluencingBoneNames())
                {
                    if (rig->getVerticesInfluencedBy(name).empty())
                        continue;
                    const auto* node = getNode(name);
                    bool descendant = node && node == physicalBone;
                    if (node && physicalBone && !descendant)
                        for (const auto& bonePath : node->getParentalNodePaths())
                            if (std::find(bonePath.begin(), bonePath.end(), physicalBone) != bonePath.end())
                            {
                                descendant = true;
                                break;
                            }
                    if (!descendant)
                        unsupported.insert(name);
                }
            if (!unsupported.empty())
            {
                if (unsupportedBones)
                    unsupportedBones->emplace(bone, static_cast<unsigned>(unsupported.size()));
                continue;
            }
            std::vector<DecapitationGeometry::Snapshot> snapshots;
            DecapitationGeometry::SnapshotVisitor visitor(snapshots, boneWorld, osg::Matrixf(), true);
            visitor.snapshot(mObjectParts[part]->getNode());
            if (visitor.failed || snapshots.empty())
                continue;
            std::vector<osg::Vec3f> vertices;
            bool finite = true;
            for (const auto& snapshot : snapshots)
                for (const auto& vertex : *static_cast<osg::Vec3Array*>(snapshot.geometry->getVertexArray()))
                {
                    if (!std::isfinite(vertex.x()) || !std::isfinite(vertex.y()) || !std::isfinite(vertex.z()))
                    {
                        finite = false;
                        break;
                    }
                    vertices.push_back(vertex);
                }
            if (finite && !vertices.empty())
                result.emplace(bone, std::move(vertices));
        }
        return result;
    }

    bool NpcAnimation::setHeadSevered(bool severed)
    {
        if (!severed) { mDecapitation.reset(); return true; }
        if (mBisection || mLimbSevering) return false;
        if (mDecapitation) return true;
        int previous=mDecapitationFailure;
        bool result=rebuildSeveredHead();
        if (!result && previous!=mDecapitationFailure)
            Log(Debug::Warning)<<"Procedural head separation unavailable, stage "<<mDecapitationFailure;
        return result;
    }

    bool NpcAnimation::canSeverLegs() const
    {
        if (!mPtr.getClass().hasInventoryStore(mPtr)) return true;
        const auto& inventory = mPtr.getClass().getInventoryStore(mPtr);
        return inventory.getSlot(MWWorld::InventoryStore::Slot_Skirt) == inventory.end()
            && inventory.getSlot(MWWorld::InventoryStore::Slot_Robe) == inventory.end();
    }

    unsigned NpcAnimation::getSeveredLimbsMask() const
    {
        return mLimbSevering ? mLimbSevering->mask : 0u;
    }

    bool NpcAnimation::setLimbsSevered(unsigned mask)
    {
        if (mask & ~15u) return false;
        if (mask == 0)
        {
            if (mLimbSevering) clearCutBloodFlow();
            mLimbSevering.reset();
            mLimbReplacedParts.fill(false);
            mLimbSeveringFailure = 0;
            return true;
        }
        if (mask == getSeveredLimbsMask()) return true;
        return rebuildSeveredLimbs(mask);
    }

    bool NpcAnimation::rebuildSeveredLimbs(unsigned mask)
    {
        const auto started = std::chrono::steady_clock::now();
        mLimbSeveringFailure = 1;
        if (!mask || (mask & ~15u) || mDecapitation || mBisection || mPtr == MWMechanics::getPlayer()
            || mBodyAwareness || mNpcType == Type_Werewolf || mViewMode != VM_Normal
            || ((mask & 3u) && !mLimbSevering && !canSeverLegs())) return false;
        const auto& nodes = getNodeMap();
        using WeakBoneMap = std::unordered_map<std::string, osg::observer_ptr<osg::MatrixTransform>,
            Misc::StringUtils::CiHash, Misc::StringUtils::CiEqual>;
        auto liveNodes = std::make_shared<WeakBoneMap>();
        for (const auto& [name, node] : nodes) liveNodes->emplace(name, node.get());
        const LimbSeveringGeometry::BoneWorld liveBones = [liveNodes](std::string_view name) -> std::optional<osg::Matrixf> {
            const auto found = liveNodes->find(std::string(name));
            osg::ref_ptr<osg::MatrixTransform> node;
            if (found == liveNodes->end() || !found->second.lock(node)) return {};
            const auto paths = node->getParentalNodePaths();
            if (paths.empty()) return {};
            return osg::computeLocalToWorld(paths.front());
        };
        const auto descendant = [&](std::string_view name, std::string_view ancestor) {
            const auto found = nodes.find(std::string(name));
            if (found == nodes.end()) return false;
            const auto paths = found->second->getParentalNodePaths();
            if (paths.empty()) return false;
            return std::any_of(paths.front().begin(), paths.front().end(), [&](const auto* node) {
                return Misc::StringUtils::ciEqual(node->getName(), ancestor);
            });
        };
        std::vector<LimbSeveringGeometry::Cut> cuts;
        std::vector<std::string> nextBones;
        for (const auto& [bit, name, parent, child, next] : {
                 std::tuple{ 1u, "leftLeg", "Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot" },
                 std::tuple{ 2u, "rightLeg", "Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot" },
                 std::tuple{ 4u, "leftArm", "Bip01 L Clavicle", "Bip01 L UpperArm", "Bip01 L Forearm" },
                 std::tuple{ 8u, "rightArm", "Bip01 R Clavicle", "Bip01 R UpperArm", "Bip01 R Forearm" } })
            if (mask & bit)
            {
                if (!liveBones(parent) || !liveBones(child) || !liveBones(next)) return false;
                LimbSeveringGeometry::Cut cut;
                cut.bit = bit; cut.name = name; cut.parent = parent; cut.child = child;
                cuts.push_back(std::move(cut));
                nextBones.emplace_back(next);
            }
        const LimbSeveringGeometry::BoneRegion boneRegion = [&](std::string_view name) {
            for (unsigned i = 0; i < cuts.size(); ++i)
                if (descendant(name, cuts[i].child)) return i + 1;
            return 0u;
        };
        const LimbSeveringGeometry::BoneTargets boneTargets = [&](std::string_view name) {
            unsigned targets = 0;
            for (const auto& cut : cuts) if (descendant(name, cut.parent)) targets |= cut.bit;
            return targets;
        };
        const LimbSeveringGeometry::BoneTargets distalTargets = [&](std::string_view name) {
            unsigned targets = 0;
            for (const auto& cut : cuts)
                if (!Misc::StringUtils::ciEqual(name, cut.child) && descendant(name, cut.child)) targets |= cut.bit;
            return targets;
        };
        // Reconstruct each original joint while rebuilding equipment on an already
        // detached ragdoll. Replacement coordinates are then attached to the live
        // fragment bone, so there is no teleport or double application of its pose.
        const LimbSeveringGeometry::BoneWorld posedBones = [&](std::string_view name) -> std::optional<osg::Matrixf> {
            auto result = liveBones(name);
            if (result && mLimbSevering)
                for (const auto& previous : mLimbSevering->cuts)
                    if (descendant(name, previous.child))
                    {
                        const auto actual = liveBones(previous.child), parent = liveBones(previous.parent);
                        if (actual && parent)
                            *result *= osg::Matrixf::inverse(*actual) * previous.childInParent * *parent;
                        break;
                    }
            return result;
        };
        for (std::size_t i = 0; i < cuts.size(); ++i)
        {
            auto& cut = cuts[i];
            cut.parentWorld = *posedBones(cut.parent);
            cut.childWorld = *posedBones(cut.child);
            cut.normal = posedBones(nextBones[i])->getTrans() - cut.childWorld.getTrans();
            cut.length = cut.normal.normalize();
            if (cut.length < .1f) return false;
            // The cut lies just inside the calf/upper arm. This includes modular
            // skin and armor whose proximal end is exactly on the joint seam.
            cut.distance = std::clamp(cut.length * .075f, .75f, 2.5f);
            if (mLimbSevering)
                for (const auto& previous : mLimbSevering->cuts)
                    if (previous.bit == cut.bit) cut.distance = previous.distance;
            cut.center = cut.childWorld.getTrans() + cut.normal * cut.distance;
        }
        try
        {
            mLimbSeveringFailure = 2;
            std::vector<DecapitationGeometry::Snapshot> snapshots;
            std::array<bool, ESM::PRT_Count> replaced{};
            for (int part = 0; part < ESM::PRT_Count; ++part)
            {
                if (!mObjectParts[part] || part == ESM::PRT_Head || part == ESM::PRT_Hair
                    || part == ESM::PRT_Shield || part == ESM::PRT_Weapon) continue;
                bool relevant = false;
                // A rigged full-body garment can live in any part slot. Inspect
                // its actual bones so that no skin bridge survives the cut.
                DecapitationGeometry::RetainedRigVisitor retained;
                retained.setNodeMaskOverride(~0u);
                retained.snapshot(mObjectParts[part]->getNode());
                for (const auto& [rig, path] : retained.rigs)
                    for (const auto& name : rig->getInfluencingBoneNames())
                        if (boneTargets(name) && !rig->getVerticesInfluencedBy(name).empty()) relevant = true;
                const auto paths = mObjectParts[part]->getNode()->getParentalNodePaths();
                if (!paths.empty())
                    for (const auto* node : paths.front()) if (boneTargets(node->getName())) relevant = true;
                if (!relevant) continue;
                const std::size_t first = snapshots.size();
                DecapitationGeometry::SnapshotVisitor visitor(snapshots, posedBones, osg::Matrixf::identity(), false);
                visitor.snapshot(mObjectParts[part]->getNode());
                if (visitor.failed) return false;
                for (std::size_t i = first; i < snapshots.size(); ++i)
                {
                    auto& snapshot = snapshots[i];
                    if (dynamic_cast<SceneUtil::RigGeometry*>(snapshot.sourceDrawable.get())) continue;
                    for (auto node = snapshot.sourcePath.rbegin(); node != snapshot.sourcePath.rend(); ++node)
                        if (const auto actual = liveBones((*node)->getName()))
                        {
                            const auto posed = posedBones((*node)->getName());
                            const auto correction = osg::Matrixf::inverse(*actual) * *posed;
                            for (auto& p : *static_cast<osg::Vec3Array*>(snapshot.geometry->getVertexArray())) p = p * correction;
                            if (auto* normals = dynamic_cast<osg::Vec3Array*>(snapshot.geometry->getNormalArray()))
                                for (auto& n : *normals) n = osg::Matrixf::transform3x3(n, correction);
                            if (auto* tangents = dynamic_cast<osg::Vec4Array*>(snapshot.geometry->getTexCoordArray(7)))
                                for (auto& t : *tangents) t = osg::Vec4f(osg::Matrixf::transform3x3(
                                    osg::Vec3f(t.x(), t.y(), t.z()), correction), t.w());
                            break;
                        }
                }
                replaced[part] = snapshots.size() > first;
            }
            if (!prewarmDecapitationMaterial()) { mLimbSeveringFailure = 11; return false; }
            auto prepared = std::make_unique<LimbSeveringGeometry>();
            if (!prepared->build(snapshots, cuts, posedBones, liveBones, boneRegion, boneTargets, distalTargets,
                    mDecapitationCapState, mDecapitationHeadCapState))
            {
                mLimbSeveringFailure = prepared->failure;
                Log(Debug::Warning) << "Procedural limb separation unavailable, stage " << mLimbSeveringFailure;
                return false;
            }
            transferBloodDecalsToLimbs(*prepared);
            for (const auto& region : prepared->regions) mResourceSystem->getSceneManager()->recreateShaders(region);
            for (const auto& cut : prepared->cuts)
                for (const auto& cap : cut.caps) mResourceSystem->getSceneManager()->recreateShaders(cap);
            prepared->prepareDrawBuffers();
            // Stage every attachment while hidden. Allocation or attachment
            // failure destroys only this candidate; the old visible state remains.
            for (const auto& anchor : prepared->anchors) if (nodes.find(anchor) == nodes.end()) return false;
            for (const auto& cut : prepared->cuts)
                if (nodes.find(cut.parent) == nodes.end() || nodes.find(cut.child) == nodes.end()) return false;
            for (int part = 0; part < ESM::PRT_Count; ++part)
                if (replaced[part] && mObjectParts[part])
                    prepared->originals.emplace_back(mObjectParts[part]->getNode(), 0);
            for (std::size_t i = 0; i < prepared->regions.size(); ++i)
            {
                prepared->regions[i]->setNodeMask(0);
                if (!nodes.at(prepared->anchors[i])->addChild(prepared->regions[i])) return false;
            }
            for (const auto& cut : prepared->cuts)
            {
                cut.caps[0]->setNodeMask(0); cut.caps[1]->setNodeMask(0);
                if (!nodes.at(cut.parent)->addChild(cut.caps[0])
                    || !nodes.at(cut.child)->addChild(cut.caps[1])) return false;
            }
            if (mLimbSevering) mLimbSevering->restore();
            clearRagdollGarmentCoverage();
            for (auto& [node, oldMask] : prepared->originals)
            {
                oldMask = node->getNodeMask();
                node->setNodeMask(0);
            }
            for (const auto& region : prepared->regions) region->setNodeMask(~0u);
            for (const auto& cut : prepared->cuts)
                for (const auto& cap : cut.caps) cap->setNodeMask(~0u);
            prepared->committed = true;
            prepared->preparationMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            mLimbReplacedParts = replaced;
            mLimbSevering = std::move(prepared);
            mLimbSeveringFailure = 0;
            return true;
        }
        catch (const std::exception& error)
        {
            Log(Debug::Warning) << "Procedural limb separation rejected: " << error.what();
            return false;
        }
    }

    std::map<std::string, double> NpcAnimation::getLimbSeveringStats() const
    {
        std::map<std::string, double> out{ { "mask", getSeveredLimbsMask() },
            { "severed", mLimbSevering ? 1. : 0. }, { "failure", mLimbSeveringFailure } };
        if (!mLimbSevering) return out;
        const auto& cut = *mLimbSevering;
        out["sourceTriangles"] = cut.sourceTriangles;
        out["resultTriangles"] = cut.resultTriangles;
        out["capTriangles"] = cut.capTriangles;
        out["capLoops"] = cut.capLoops;
        out["seamFallbacks"] = cut.seamFallbacks;
        out["articulatedSurfaces"] = cut.articulatedSurfaces;
        out["decalTriangles"] = cut.decalTriangles;
        out["reconstructionError"] = cut.reconstructionError;
        out["preparationMilliseconds"] = cut.preparationMilliseconds;
        out["replacedParts"] = cut.originals.size();
        unsigned replacedPartMask = 0;
        for (unsigned part = 0; part < ESM::PRT_Count; ++part)
            if (mLimbReplacedParts[part]) replacedPartMask |= 1u << part;
        out["replacedPartMask"] = replacedPartMask;
        for (const auto& limb : cut.cuts)
        {
            out[limb.name + "CutDistance"] = limb.distance;
            out[limb.name + "StumpRadius"] = limb.surfaceRadii[0];
            out[limb.name + "FragmentRadius"] = limb.surfaceRadii[1];
            out[limb.name + "StumpAttached"] = limb.caps[0]->getNumParents();
            out[limb.name + "FragmentAttached"] = limb.caps[1]->getNumParents();
        }
        return out;
    }

    std::map<std::string, osg::Vec3f> NpcAnimation::getLimbSeveringSurfacesForAudit() const
    {
        std::map<std::string, osg::Vec3f> out;
        if (!mLimbSevering) return out;
        for (const auto& cut : mLimbSevering->cuts)
            for (unsigned side = 0; side < 2; ++side)
            {
                const auto world = getBoneWorldMatrix(side ? cut.child : cut.parent);
                if (!world) continue;
                const auto u = osg::Matrixf::transform3x3(cut.surfaceU[side], *world),
                    v = osg::Matrixf::transform3x3(cut.surfaceV[side], *world);
                auto normal = u ^ v;
                normal.normalize();
                const auto prefix = cut.name + (side ? "Fragment" : "Stump");
                out[prefix + "Center"] = cut.surfaceCenters[side] * *world;
                out[prefix + "Normal"] = normal;
                out[prefix + "Radius"] = osg::Vec3f(cut.surfaceRadii[side], 0, 0);
            }
        return out;
    }

    bool NpcAnimation::setTorsoSevered(bool severed)
    {
        if (!severed)
        {
            if (mBisection) clearCutBloodFlow();
            mBisection.reset();
            return true;
        }
        if (mBisection) return true;
        return rebuildSeveredTorso();
    }

    bool NpcAnimation::rebuildSeveredTorso()
    {
        const auto started = std::chrono::steady_clock::now();
        mBisectionFailure = 1;
        if (mDecapitation || mLimbSevering || mPtr == MWMechanics::getPlayer() || mBodyAwareness
            || mNpcType == Type_Werewolf || mViewMode != VM_Normal)
            return false;
        const auto pelvis = getBoneWorldMatrix("Bip01 Pelvis"), actualSpine = getBoneWorldMatrix("Bip01 Spine1"),
            actualNeck = getBoneWorldMatrix("Bip01 Neck");
        const auto& nodes = getNodeMap();
        const auto lowerNode = nodes.find("Bip01 Pelvis"), upperNode = nodes.find("Bip01 Spine1");
        if (!pelvis || !actualSpine || !actualNeck || lowerNode == nodes.end() || upperNode == nodes.end())
            return false;
        const osg::Matrixf spine = mBisection ? mBisection->spineInPelvis * *pelvis : *actualSpine;
        const osg::Matrixf correction = osg::Matrixf::inverse(*actualSpine) * spine;
        osg::Vec3f axis = ((*actualNeck) * correction).getTrans() - pelvis->getTrans();
        if (axis.normalize() < .1f) return false;
        using WeakBoneMap = std::unordered_map<std::string, osg::observer_ptr<osg::MatrixTransform>,
            Misc::StringUtils::CiHash, Misc::StringUtils::CiEqual>;
        auto liveNodes = std::make_shared<WeakBoneMap>();
        for (const auto& [name, node] : nodes) liveNodes->emplace(name, node.get());
        // Cull may outlive the animation owner while the renderer releases the
        // previous frame. Never retain a callback into a raw NpcAnimation here.
        const BisectionGeometry::BoneWorld liveBoneWorld = [liveNodes](std::string_view name) -> std::optional<osg::Matrixf> {
            const auto found = liveNodes->find(std::string(name));
            osg::ref_ptr<osg::MatrixTransform> node;
            if (found == liveNodes->end() || !found->second.lock(node)) return {};
            const auto paths = node->getParentalNodePaths();
            if (paths.empty()) return {};
            return osg::computeLocalToWorld(paths.front());
        };
        const BisectionGeometry::BoneSide boneSide = [&](std::string_view name) {
            const auto found = nodes.find(std::string(name));
            if (found == nodes.end()) return -1;
            const auto paths = found->second->getParentalNodePaths();
            if (paths.empty()) return -1;
            for (const auto* node : paths.front())
                if (Misc::StringUtils::ciEqual(node->getName(), "Bip01 Spine1")) return 1;
            return 0;
        };
        const BisectionGeometry::BoneWorld boneWorld = [&](std::string_view name) -> std::optional<osg::Matrixf> {
            auto matrix = liveBoneWorld(name);
            if (matrix && boneSide(name) == 1) *matrix *= correction;
            return matrix;
        };
        const BisectionGeometry::LimbSide limbSide = [&](std::string_view name) {
            const auto found = nodes.find(std::string(name));
            if (found == nodes.end()) return -1;
            const auto paths = found->second->getParentalNodePaths();
            if (paths.empty()) return -1;
            for (const auto* node : paths.front())
            {
                for (const auto* upper : { "Bip01 L Clavicle", "Bip01 R Clavicle", "Bip01 L UpperArm",
                         "Bip01 R UpperArm", "Bip01 Neck" })
                    if (Misc::StringUtils::ciEqual(node->getName(), upper)) return 1;
                for (const auto* lower : { "Bip01 L Thigh", "Bip01 R Thigh", "Bip01 Tail" })
                    if (Misc::StringUtils::ciEqual(node->getName(), lower)) return 0;
            }
            return -1;
        };
        try
        {
            mBisectionFailure = 2;
            std::vector<DecapitationGeometry::Snapshot> snapshots;
            // A custom full-body mesh installed in an extremity slot must not
            // remain as an uncut skin bridge. Ordinary modular limbs stay on
            // their existing bone branch and do not enter the clipping path.
            for (int part = 0; part < ESM::PRT_Count; ++part)
                if (part != ESM::PRT_Cuirass && part != ESM::PRT_Groin && part != ESM::PRT_Skirt
                    && mObjectParts[part])
                {
                    DecapitationGeometry::RetainedRigVisitor retained;
                    retained.snapshot(mObjectParts[part]->getNode());
                    for (const auto& [rig, path] : retained.rigs)
                    {
                        unsigned branches = 0;
                        for (const auto& name : rig->getInfluencingBoneNames())
                            if (!rig->getVerticesInfluencedBy(name).empty())
                            {
                                const int side = boneSide(name);
                                if (side < 0) { mBisectionFailure = 13; return false; }
                                branches |= 1u << side;
                            }
                        if (branches == 3) { mBisectionFailure = 13; return false; }
                    }
                }
            for (auto part : { ESM::PRT_Cuirass, ESM::PRT_Groin, ESM::PRT_Skirt })
                if (mObjectParts[part])
                {
                    DecapitationGeometry::SnapshotVisitor visitor(snapshots, boneWorld, osg::Matrixf::identity(), false);
                    const std::size_t first = snapshots.size();
                    visitor.snapshot(mObjectParts[part]->getNode());
                    if (visitor.failed) return false;
                    for (std::size_t i = first; i < snapshots.size(); ++i)
                    {
                        auto& snapshot = snapshots[i];
                        if (dynamic_cast<SceneUtil::RigGeometry*>(snapshot.sourceDrawable.get())) continue;
                        const bool upper = std::any_of(snapshot.sourcePath.begin(), snapshot.sourcePath.end(),
                            [](const auto* node) { return Misc::StringUtils::ciEqual(node->getName(), "Bip01 Spine1"); });
                        if (!upper) continue;
                        for (auto& p : *static_cast<osg::Vec3Array*>(snapshot.geometry->getVertexArray())) p = p * correction;
                        if (auto* normals = dynamic_cast<osg::Vec3Array*>(snapshot.geometry->getNormalArray()))
                            for (auto& n : *normals) n = osg::Matrixf::transform3x3(n, correction);
                        if (auto* tangents = dynamic_cast<osg::Vec4Array*>(snapshot.geometry->getTexCoordArray(7)))
                            for (auto& t : *tangents) t = osg::Vec4f(osg::Matrixf::transform3x3(
                                osg::Vec3f(t.x(), t.y(), t.z()), correction), t.w());
                    }
                }
            if (!prewarmDecapitationMaterial()) { mBisectionFailure = 11; return false; }
            std::unique_ptr<BisectionGeometry> prepared;
            // Strict intersections can coincide with a source seam or vertex.
            // Search only inside a narrow anatomical band around the waist joint.
            for (float offset : { 0.f, .125f, -.125f, .5f, -.5f, 1.f, -1.f, 2.f, -2.f })
            {
                auto candidate = std::make_unique<BisectionGeometry>();
                if (candidate->build(snapshots, boneWorld, liveBoneWorld, boneSide, limbSide, *pelvis, spine, axis,
                        offset, mDecapitationCapState, mDecapitationHeadCapState))
                { prepared = std::move(candidate); break; }
                mBisectionFailure = candidate->failure;
            }
            if (!prepared)
            {
                Log(Debug::Warning) << "Procedural waist separation unavailable, stage " << mBisectionFailure;
                return false;
            }
            transferBloodDecalsToTorso(*prepared);
            mResourceSystem->getSceneManager()->recreateShaders(prepared->lower);
            mResourceSystem->getSceneManager()->recreateShaders(prepared->upper);
            prepared->prepareDrawBuffers();
            // Geometry, contours, materials, and both draw buffers are ready.
            // This is the only mutation of the actor's visible source graph.
            if (mBisection) mBisection->restore();
            for (auto part : { ESM::PRT_Cuirass, ESM::PRT_Groin, ESM::PRT_Skirt })
                if (mObjectParts[part])
                {
                    auto node = mObjectParts[part]->getNode();
                    prepared->originals.emplace_back(node, node->getNodeMask());
                }
            clearRagdollGarmentCoverage();
            for (const auto& [node, mask] : prepared->originals) node->setNodeMask(0);
            lowerNode->second->addChild(prepared->lower);
            upperNode->second->addChild(prepared->upper);
            prepared->committed = true;
            prepared->preparationMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            mBisection = std::move(prepared);
            mBisectionFailure = 0;
            return true;
        }
        catch (const std::exception& error)
        {
            Log(Debug::Warning) << "Procedural waist separation rejected: " << error.what();
            return false;
        }
    }

    std::map<std::string, double> NpcAnimation::getBisectionStats() const
    {
        std::map<std::string, double> out{ { "severed", mBisection ? 1. : 0. }, { "failure", mBisectionFailure } };
        if (mBisection)
        {
            const auto& cut = *mBisection;
            out["sourceTriangles"] = cut.sourceTriangles;
            out["resultTriangles"] = cut.resultTriangles;
            out["capTriangles"] = cut.capTriangles;
            out["capLoops"] = cut.capLoops;
            out["protectedTriangles"] = cut.protectedTriangles;
            out["articulatedSurfaces"] = cut.articulatedSurfaces;
            out["decalTriangles"] = cut.decalTriangles;
            out["cutDistance"] = cut.cutDistance;
            out["surfaceRadius"] = cut.surfaceRadius;
            out["reconstructionError"] = cut.reconstructionError;
            out["preparationMilliseconds"] = cut.preparationMilliseconds;
            out["lowerAttached"] = cut.lower->getNumParents();
            out["upperAttached"] = cut.upper->getNumParents();
            out["replacedParts"] = cut.originals.size();
        }
        return out;
    }

    std::map<std::string, osg::Vec3f> NpcAnimation::getBisectionSurfacesForAudit() const
    {
        std::map<std::string, osg::Vec3f> out;
        if (!mBisection) return out;
        for (unsigned side = 0; side < 2; ++side)
        {
            const auto world = getBoneWorldMatrix(side ? "Bip01 Spine1" : "Bip01 Pelvis");
            if (!world) continue;
            const auto u = osg::Matrixf::transform3x3(mBisection->surfaceU[side], *world),
                v = osg::Matrixf::transform3x3(mBisection->surfaceV[side], *world);
            auto normal = u ^ v;
            normal.normalize();
            const std::string prefix = side ? "upper" : "lower";
            out[prefix + "Center"] = mBisection->surfaceCenters[side] * *world;
            out[prefix + "Normal"] = normal;
            out[prefix + "Radius"] = osg::Vec3f(mBisection->surfaceRadius, 0, 0);
        }
        return out;
    }

    std::map<std::string, double> NpcAnimation::getDecapitationStats() const
    {
        std::map<std::string,double> out{{"severed", mDecapitation ? 1. : 0.}, {"failure", mDecapitationFailure}};
        out["prewarmQueued"]=mDecapitationPrewarmQueued;
        out["warmDrawn"]=mDecapitationWarmDrawn;
        out["warmTimedOut"]=mDecapitationWarmTimedOut;
        out["warmDrawables"]=mDecapitationWarmDrawables;
        out["warmPending"]=mDecapitationRenderPrewarm ? 1. : 0.;
        if (mDecapitation)
        {
            out["sourceTriangles"]=mDecapitation->sourceTriangles;
            out["resultTriangles"]=mDecapitation->resultTriangles;
            out["capTriangles"]=mDecapitation->capTriangles;
            out["capLoops"]=mDecapitation->capLoops;
            out["helmetInteriorTriangles"]=mDecapitation->helmetInteriorTriangles;
            out["helmetSurfaceError"]=mDecapitation->helmetSurfaceError;
            out["cutDistance"]=mDecapitation->cutDistance;
            out["posedCutPlane"]=mDecapitation->posedCutPlane;
            out["retainedRigs"]=mDecapitation->retainedRigs.size();
            out["retainedVertices"]=mDecapitation->retainedVertices;
            out["retainedVertexMaximum"]=mDecapitation->retainedVertexMaximum;
            out["reconstructionError"]=mDecapitation->reconstructionError;
            out["preparationMilliseconds"]=mDecapitation->preparationMilliseconds;
            out["lowerAttached"]=mDecapitation->lower->getNumParents();
            out["upperAttached"]=mDecapitation->upper->getNumParents();
        }
        return out;
    }

    std::map<std::string, osg::Vec3f> NpcAnimation::getDecapitationSurfacesForAudit() const
    {
        std::map<std::string,osg::Vec3f> out;
        if(!mDecapitation)return out;
        for(int side=0;side<2;++side)
        {
            const auto world=getBoneWorldMatrix(side?"Bip01 Head":"Bip01 Neck");
            if(!world)continue;
            const auto u=osg::Matrixf::transform3x3(mDecapitation->surfaceU[side],*world);
            const auto v=osg::Matrixf::transform3x3(mDecapitation->surfaceV[side],*world);
            auto normal=u^v;normal.normalize();
            const std::string prefix=side?"upper":"lower";
            out[prefix+"Center"]=mDecapitation->surfaceCenters[side] * *world;
            out[prefix+"Normal"]=normal;
            out[prefix+"Radius"]=osg::Vec3f(mDecapitation->surfaceRadius,0,0);
        }
        return out;
    }

    bool NpcAnimation::prewarmDecapitationMaterial()
    {
        if(mDecapitationCapState&&mDecapitationHeadCapState)return true;
        if(mPtr==MWMechanics::getPlayer()||mBodyAwareness||mViewMode!=VM_Normal)return false;
        static std::mutex mutex;
        using MaterialPair=std::array<osg::observer_ptr<osg::StateSet>,2>;
        static std::map<Resource::ResourceSystem*,MaterialPair> materials;
        std::lock_guard lock(mutex);
        auto& cached=materials[mResourceSystem];
        if(cached[0].lock(mDecapitationCapState)&&cached[1].lock(mDecapitationHeadCapState))return true;
        try
        {
            std::array<osg::ref_ptr<osg::StateSet>,2> prepared;
            for(unsigned side=0;side<2;++side)
            {
                const VFS::Path::NormalizedView original(side?"textures/brutal_dm/cut_head.png":"textures/brutal_dm/cut_body.png");
                const VFS::Path::NormalizedView fallback("textures/brutal/neck_cut.png");
                const auto path=mResourceSystem->getVFS()->exists(original)?original:fallback;
                if(!mResourceSystem->getVFS()->exists(path))return false;
                osg::ref_ptr<osg::Texture2D> texture=new osg::Texture2D(mResourceSystem->getImageManager()->getImage(path));
                texture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);
                texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
                texture->setResizeNonPowerOfTwoHint(false);
                mResourceSystem->getSceneManager()->applyFilterSettings(texture);
                osg::ref_ptr<osg::StateSet> state=new osg::StateSet;
                osg::ref_ptr<osg::Material> material=new osg::Material;
                material->setDiffuse(osg::Material::FRONT_AND_BACK,osg::Vec4(1,1,1,1));
                material->setAmbient(osg::Material::FRONT_AND_BACK,osg::Vec4(.6f,.6f,.6f,1));
                state->setAttributeAndModes(material,osg::StateAttribute::ON);
                state->setMode(GL_LIGHTING,osg::StateAttribute::ON);
                state->setMode(GL_CULL_FACE,osg::StateAttribute::ON);
                state->setMode(GL_BLEND,osg::StateAttribute::OFF);
                state->setAttributeAndModes(new SceneUtil::AutoDepth);
                state->setTextureAttributeAndModes(0,texture,osg::StateAttribute::ON);
                state->setTextureAttribute(0,new SceneUtil::TextureType("diffuseMap"));
                const std::vector<DecapitationGeometry::Vertex> vertices{
                    {osg::Vec3f(0,0,0),osg::Vec3f(0,0,1),osg::Vec2f(0,0),osg::Vec4f(1,1,1,1)},
                    {osg::Vec3f(1,0,0),osg::Vec3f(0,0,1),osg::Vec2f(1,0),osg::Vec4f(1,1,1,1)},
                    {osg::Vec3f(0,1,0),osg::Vec3f(0,0,1),osg::Vec2f(0,1),osg::Vec4f(1,1,1,1)}};
                auto warm=DecapitationGeometry::makeGeometry(vertices,state,osg::Matrixf::identity());
                mResourceSystem->getSceneManager()->recreateShaders(warm);
                if(auto* compile=mResourceSystem->getSceneManager()->getIncrementalCompileOperation())
                {compile->add(warm);++mDecapitationPrewarmQueued;}
                prepared[side]=warm->getStateSet();cached[side]=prepared[side];
            }
            mDecapitationCapState=prepared[0];mDecapitationHeadCapState=prepared[1];return true;
        }
        catch(const std::exception& error)
        {Log(Debug::Warning)<<"Could not prewarm neck material: "<<error.what();return false;}
    }

    bool NpcAnimation::rebuildSeveredHead(bool prewarmOnly)
    {
        const auto preparationStart=std::chrono::steady_clock::now();
        const int previousFailure=mDecapitationFailure;
        mDecapitationFailure=1;
        if (mPtr==MWMechanics::getPlayer() || mNpcType==Type_Werewolf || mViewMode!=VM_Normal)
            return false;
        const auto neck=getBoneWorldMatrix("Bip01 Neck"), actualHead=getBoneWorldMatrix("Bip01 Head");
        const auto& nodes=getNodeMap();
        auto neckNode=nodes.find("Bip01 Neck"), headNode=nodes.find("Bip01 Head");
        if (!neck || !actualHead || neckNode==nodes.end() || headNode==nodes.end()) return false;
        const osg::Matrixf head=mDecapitation ? mDecapitation->headInNeck * *neck : *actualHead;
        const osg::Matrixf correction=osg::Matrixf::inverse(*actualHead)*head;
        const std::function<std::optional<osg::Matrixf>(std::string_view)> boneWorld=
            [&](std::string_view name)->std::optional<osg::Matrixf>
            {
                auto world=getBoneWorldMatrix(name);if(!world)return {};
                auto found=nodes.find(std::string(name));
                if(found!=nodes.end())
                {
                    auto paths=found->second->getParentalNodePaths();
                    if(!paths.empty())for(auto* node:paths.front())
                        if(Misc::StringUtils::ciEqual(node->getName(),"Bip01 Head"))
                        { *world *= correction;break; }
                }
                return world;
            };
        try
        {
            mDecapitationFailure=2;
            std::vector<DecapitationGeometry::Snapshot> snapshots;
            std::vector<ESM::PartReferenceType> snapshotParts;
            for(auto type:{ESM::PRT_Head,ESM::PRT_Hair,ESM::PRT_Neck})
                if(mObjectParts[type])
                {
                    const bool wholeHeadPart=type==ESM::PRT_Hair
                        || mPartslots[type]==MWWorld::InventoryStore::Slot_Helmet;
                    DecapitationGeometry::SnapshotVisitor visitor(snapshots,boneWorld,correction,wholeHeadPart,
                        mPartslots[type]==MWWorld::InventoryStore::Slot_Helmet);
                    visitor.snapshot(mObjectParts[type]->getNode());
                    if(visitor.failed)return false;
                    snapshotParts.resize(snapshots.size(),type);
                }
            if(snapshots.empty())return false;
            mDecapitationFailure=11;
            if(!prewarmDecapitationMaterial())return false;
            const auto state=mDecapitationCapState;
            auto prepared=std::make_unique<DecapitationGeometry>();
            osg::Vec3f cutAxis=head.getTrans()-neck->getTrans();
            const float neckLength=cutAxis.normalize();
            if(neckLength<.1f){mDecapitationFailure=3;return false;}
            const osg::Matrixf neckInverse=osg::Matrixf::inverse(*neck);
            DecapitationGeometry::RetainedRigVisitor retained;
            std::vector<osg::Vec3f> retainedPoints;
            const bool auditRejection = !prewarmOnly && decapitationAuditEnabled();
            std::vector<osg::ref_ptr<osg::Geometry>> retainedAuditSurfaces;
            for(int part=0;part<ESM::PRT_Count;++part)
                if(part!=ESM::PRT_Head && part!=ESM::PRT_Hair && part!=ESM::PRT_Neck && mObjectParts[part])
                    retained.snapshot(mObjectParts[part]->getNode());
            for(const auto& [rig,path]:retained.rigs)
            {
                SceneUtil::RigGeometry::BonePoseOverrides overrides;
                std::set<unsigned short> affected;
                for(const auto& name:rig->getInfluencingBoneNames())
                {
                    auto bone=nodes.find(name);if(bone==nodes.end())continue;
                    auto paths=bone->second->getParentalNodePaths();if(paths.empty())continue;
                    const bool followsHead=std::any_of(paths.front().begin(),paths.front().end(),[](const osg::Node* node){
                        return Misc::StringUtils::ciEqual(node->getName(),"Bip01 Head");});
                    if(!followsHead)continue;
                    const auto vertices=rig->getVerticesInfluencedBy(name);
                    if(vertices.empty())continue;
                    auto world=boneWorld(name);if(!world)
                    {mDecapitationFailure=13;if(!prewarmOnly && previousFailure!=13)Log(Debug::Warning)<<"Decapitation retained bone unavailable: "<<name;return false;}
                    overrides.emplace(name,SceneUtil::RigGeometry::BonePoseOverride{"Bip01 Neck",*world*neckInverse,nullptr});
                    affected.insert(vertices.begin(),vertices.end());
                }
                if(overrides.empty())continue;
                const auto snapshot=rig->snapshotWorldGeometry(path,boneWorld);
                if(!snapshot)
                {mDecapitationFailure=13;if(!prewarmOnly && previousFailure!=13)Log(Debug::Warning)<<"Decapitation retained rig snapshot unavailable";return false;}
                const auto* vertices=static_cast<const osg::Vec3Array*>(snapshot->getVertexArray());
                if (auditRejection)
                    retainedAuditSurfaces.push_back(snapshot);
                for(auto index:affected)
                {
                    if(index>=vertices->size())
                    {mDecapitationFailure=13;if(!prewarmOnly && previousFailure!=13)Log(Debug::Warning)<<"Decapitation retained vertex out of range: "<<index<<"/"<<vertices->size();return false;}
                    prepared->retainedVertexMaximum=std::max(prepared->retainedVertexMaximum,
                        ((*vertices)[index]-neck->getTrans())*cutAxis);
                    retainedPoints.push_back((*vertices)[index]);
                }
                prepared->retainedVertices+=affected.size();
                prepared->retainedRigs.push_back({rig,std::move(overrides)});
            }
            // Keep the plane above retained body skin. The usual bone-distance limit is
            // conservative, but a bent loaded death pose can place valid neck skin past it.
            // In that case require the actual Neck surface to cross the plane, with all
            // non-helmet Head geometry strictly above it; never move the cut into the chin.
            float cutDistance=std::max(neckLength*.5f,prepared->retainedVertexMaximum+.15f);
            if(cutDistance>neckLength*.9f)
            {
                float neckMinimum=std::numeric_limits<float>::max();
                float neckMaximum=std::numeric_limits<float>::lowest();
                float headMinimum=std::numeric_limits<float>::max();
                std::vector<osg::Vec3f> neckPoints,headPoints;
                std::vector<std::array<osg::Vec3f,3>> neckTriangles;
                AnatomicalCutSearchDiagnostics searchDiagnostics;
                for(std::size_t i=0;i<snapshots.size();++i)
                {
                    const auto& snapshot=snapshots[i];
                    if(snapshot.hair || snapshot.helmet)continue;
                    if(snapshotParts[i]!=ESM::PRT_Head && snapshotParts[i]!=ESM::PRT_Neck)continue;
                    const auto* vertices=dynamic_cast<const osg::Vec3Array*>(snapshot.geometry->getVertexArray());
                    if(!vertices)continue;
                    for(const auto& vertex:*vertices)
                    {
                        const float distance=(vertex-neck->getTrans())*cutAxis;
                        if(snapshotParts[i]==ESM::PRT_Neck)
                        {
                            neckPoints.push_back(vertex);
                            neckMinimum=std::min(neckMinimum,distance);
                            neckMaximum=std::max(neckMaximum,distance);
                        }
                        else
                        {
                            headPoints.push_back(vertex);
                            headMinimum=std::min(headMinimum,distance);
                        }
                    }
                }
                // A helmet may replace the flesh Head part entirely. Its complete mesh
                // stays with the head; the Head bone origin remains the upper limit.
                const float ceiling=std::min({neckMaximum,headMinimum,neckLength});
                bool insideNeck=cutDistance>neckMinimum+.05f && cutDistance<ceiling-.05f;
                if(!insideNeck)
                {
                    // Head flesh was required to lie wholly above every candidate
                    // plane; hair and helmets remain whole. Only Neck triangles can
                    // contribute a cut contour. Cache them once for bounded graph
                    // validation instead of repeatedly building complete geometry.
                    for(std::size_t i=0;i<snapshots.size();++i)
                    {
                        const auto& snapshot=snapshots[i];
                        if(snapshotParts[i]!=ESM::PRT_Neck || snapshot.hair || snapshot.helmet)continue;
                        const auto* vertices=dynamic_cast<const osg::Vec3Array*>(snapshot.geometry->getVertexArray());
                        if(!vertices)continue;
                        osg::TriangleIndexFunctor<DecapitationGeometry::TriangleList> triangles;
                        snapshot.geometry->accept(triangles);
                        for(const auto& triangle:triangles.triangles)
                        {
                            if(triangle[0]>=vertices->size() || triangle[1]>=vertices->size()
                                || triangle[2]>=vertices->size())return false;
                            neckTriangles.push_back({(*vertices)[triangle[0]],(*vertices)[triangle[1]],(*vertices)[triangle[2]]});
                        }
                    }
                    AnatomicalCutPlane posed;
                    if(selectAnatomicalCutPlane(neck->getTrans(),head.getTrans(),
                        osg::Matrixf::transform3x3(osg::Vec3f(1,0,0),head),
                        osg::Matrixf::transform3x3(osg::Vec3f(1,0,0),*neck),
                        osg::Matrixf::transform3x3(osg::Vec3f(0,1,0),head),
                        retainedPoints,neckPoints,headPoints,posed,
                        [&](const osg::Vec3f& normal,float distance){
                            return hasClosedCutContour(neckTriangles,neck->getTrans()+normal*distance,normal);
                        }, &searchDiagnostics))
                    {
                        cutAxis=posed.normal;
                        cutDistance=posed.distance;
                        prepared->retainedVertexMaximum=posed.retainedMaximum;
                        prepared->posedCutPlane=true;
                        neckMinimum=posed.neckMinimum;
                        neckMaximum=posed.neckMaximum;
                        headMinimum=posed.headMinimum;
                        insideNeck=true;
                        if(!prewarmOnly)
                            Log(Debug::Info)<<"Decapitation posed plane accepted: clearance="<<posed.clearance
                                <<" normal="<<cutAxis.x()<<","<<cutAxis.y()<<","<<cutAxis.z();
                    }
                }
                if(!prewarmOnly && (previousFailure!=13 || insideNeck))
                    Log(insideNeck?Debug::Info:Debug::Warning)<<"Decapitation anatomical plane "
                        <<(insideNeck?"accepted":"rejected")<<": neckLength="<<neckLength
                        <<" retainedMaximum="<<prepared->retainedVertexMaximum<<" cutDistance="<<cutDistance
                        <<" neckRange="<<neckMinimum<<","<<neckMaximum<<" headMinimum="<<headMinimum;
                if(!insideNeck)
                {
                    mDecapitationFailure=13;
                    if (!prewarmOnly && previousFailure != 13)
                        Log(Debug::Warning) << "Decapitation plane search rejected: candidates="
                            << searchDiagnostics.normalCandidates << " axisRejected=" << searchDiagnostics.axisRejected
                            << " intervalRejected=" << searchDiagnostics.intervalRejected
                            << " contourTests=" << searchDiagnostics.contourTests
                            << " contourRejected=" << searchDiagnostics.contourRejected
                            << " bestClearance=" << searchDiagnostics.bestClearance
                            << " fallbackNormals=" << searchDiagnostics.fallbackNormalCandidates
                            << " fallbackContourTests=" << searchDiagnostics.fallbackContourTests
                            << " fallbackSlabTests=" << searchDiagnostics.fallbackSlabTests
                            << " budgetExhausted=" << searchDiagnostics.fallbackBudgetExhausted
                            << " invalidInput=" << searchDiagnostics.invalidInput;
                    if (auditRejection)
                        dumpRejectedDecapitation(*neck, head, retainedPoints, neckPoints, headPoints, neckTriangles,
                            snapshots, snapshotParts, retainedAuditSurfaces, searchDiagnostics);
                    if(!prewarmOnly && previousFailure!=13)
                        Log(Debug::Warning)<<"Decapitation retained-skin plane rejected: neckLength="<<neckLength
                            <<" retainedMaximum="<<prepared->retainedVertexMaximum<<" cutDistance="<<cutDistance
                            <<" retainedRigs="<<prepared->retainedRigs.size()<<" retainedVertices="<<prepared->retainedVertices
                            <<" neck="<<neck->getTrans().x()<<","<<neck->getTrans().y()<<","<<neck->getTrans().z()
                            <<" head="<<head.getTrans().x()<<","<<head.getTrans().y()<<","<<head.getTrans().z();
                    return false;
                }
            }
            prepared->lower->setName("Brutal Decapitation Lower");
            prepared->upper->setName("Brutal Decapitation Upper");
            if(!prepared->build(snapshots,*neck,head,state,cutDistance,cutAxis,mDecapitationHeadCapState))
            {
                mDecapitationFailure=prepared->failure?prepared->failure:12;
                if(!prewarmOnly && previousFailure!=mDecapitationFailure)
                {
                Log(Debug::Warning)<<"Decapitation contour diagnostic stage="<<mDecapitationFailure
                    <<" sourceTriangles="<<prepared->sourceTriangles
                    <<" plane="<<prepared->diagnosticCenter.x()<<","<<prepared->diagnosticCenter.y()<<","<<prepared->diagnosticCenter.z()
                    <<" normal="<<prepared->diagnosticNormal.x()<<","<<prepared->diagnosticNormal.y()<<","<<prepared->diagnosticNormal.z();
                for(const auto& range:prepared->diagnosticRanges)
                    Log(Debug::Warning)<<"Decapitation snapshot plane range="<<range.x()<<","<<range.y()<<" triangles="<<range.z();
                }
                return false;
            }
            mResourceSystem->getSceneManager()->recreateShaders(prepared->lower);
            mResourceSystem->getSceneManager()->recreateShaders(prepared->upper);
            if(!prewarmOnly)transferBloodDecalsToHead(*prepared, true);
            if(prewarmOnly)
            {
                if(auto* compile=mResourceSystem->getSceneManager()->getIncrementalCompileOperation())
                {
                    compile->add(prepared->lower);compile->add(prepared->upper);
                    mDecapitationPrewarmQueued+=2;
                }
                osg::ref_ptr<osg::Group> preview=new osg::Group;
                preview->addChild(prepared->lower);preview->addChild(prepared->upper);
                // Keep at most one pending preview per NPC when equipment changes.
                mDecapitationRenderPrewarm.reset();
                mDecapitationRenderPrewarm=ClippedRenderPrewarm::attach(preview,mObjectRoot);
                mDecapitationWarmDrawables=mDecapitationRenderPrewarm ? mDecapitationRenderPrewarm->drawableCount() : 0;
                return true;
            }
            // All allocation, decoding and contour validation finished before any visible mutation.
            if(mDecapitation)mDecapitation->restore();
            for(auto type:{ESM::PRT_Head,ESM::PRT_Hair,ESM::PRT_Neck})
                if(mObjectParts[type])
                {
                    auto node=mObjectParts[type]->getNode();
                    prepared->originals.emplace_back(node,node->getNodeMask());node->setNodeMask(0);
                }
            neckNode->second->addChild(prepared->lower);
            headNode->second->addChild(prepared->upper);
            prepared->committed=true;
            for(auto& retained:prepared->retainedRigs)
                retained.geometry->setBonePoseOverrides(std::move(retained.poses));
            prepared->preparationMilliseconds=std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-preparationStart).count();
            mDecapitation=std::move(prepared);
            mDecapitationFailure=0;
            return true;
        }
        catch(const std::exception& error)
        { if(!prewarmOnly)Log(Debug::Warning)<<"Procedural head separation rejected: "<<error.what();return false; }
    }

    void NpcAnimation::removeIndividualPart(ESM::PartReferenceType type)
    {
        if (type == ESM::PRT_Skirt || type == ESM::PRT_LKnee || type == ESM::PRT_RKnee)
            clearRagdollGarmentCoverage();
        mPartPriorities[type] = 0;
        mPartslots[type] = -1;

        mObjectParts[type].reset();
        mPartModels[type].clear();
        mFirstPersonWristCoverage[type].clear();
        if (mSounds[type] != nullptr && !mSoundsDisabled)
        {
            MWBase::Environment::get().getSoundManager()->stopSound(mSounds[type]);
            mSounds[type] = nullptr;
        }
    }

    void NpcAnimation::reserveIndividualPart(ESM::PartReferenceType type, int group, int priority)
    {
        if (priority > mPartPriorities[type])
        {
            removeIndividualPart(type);
            mPartPriorities[type] = priority;
            mPartslots[type] = group;
        }
    }

    void NpcAnimation::removePartGroup(int group)
    {
        for (int i = 0; i < ESM::PRT_Count; i++)
        {
            if (mPartslots[i] == group)
                removeIndividualPart((ESM::PartReferenceType)i);
        }
    }

    bool NpcAnimation::isFemalePart(const ESM::BodyPart* bodypart)
    {
        return bodypart->mData.mFlags & ESM::BodyPart::BPF_Female;
    }

    namespace
    {
        class RobeRigVisitor : public osg::NodeVisitor
        {
        public:
            RobeRigVisitor() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
            void apply(osg::Drawable& drawable) override
            {
                if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                    mRigs.push_back(rig);
            }
            std::vector<SceneUtil::RigGeometry*> mRigs;
        };
    }

    void NpcAnimation::fitFirstPersonWrists()
    {
        // The robe reserves Forearm, but a separate rigid Wrist skin still survives under its
        // fitted sleeve. Keep the exposed distal wrist and a one-unit overlap inside the cuff.
        RobeRigVisitor robe;
        mObjectParts[ESM::PRT_Cuirass]->getNode()->accept(robe);
        for (const auto& [part, forearm, bone] : {
                 std::tuple{ESM::PRT_LWrist, ESM::PRT_LForearm, "Bip01 L Forearm"},
                 std::tuple{ESM::PRT_RWrist, ESM::PRT_RForearm, "Bip01 R Forearm"} })
        {
            if (!mObjectParts[part] || mPartslots[part] != -1 || !mFirstPersonWristCoverage[part].empty()
                || mPartslots[forearm] != MWWorld::InventoryStore::Slot_Robe)
                continue;
            std::optional<float> end;
            for (const auto* rig : robe.mRigs)
                if (const auto x = rig->getBoneLocalMaxX(bone); x && (!end || *x > *end))
                    end = x;
            if (!end)
                continue; // Sleeveless or unsupported garment: preserve its ordinary skin.
            class WristVisitor : public osg::NodeVisitor
            {
            public:
                std::string_view bone;
                float end;
                std::vector<std::tuple<osg::ref_ptr<osg::Group>, osg::ref_ptr<osg::Group>, osg::ref_ptr<osg::Geometry>>> surfaces;
                WristVisitor(std::string_view b, float x) : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), bone(b), end(x) {}
                void apply(osg::Drawable& drawable) override
                {
                    auto* geometry = drawable.asGeometry();
                    if (!geometry || dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                        return;
                    const auto& path = getNodePath();
                    const auto start = std::find_if(path.begin(), path.end(),
                        [&](const auto* node){ return Misc::StringUtils::ciEqual(node->getName(), bone); });
                    if (start == path.end() || path.size() < 2)
                        return;
                    auto* parent = path[path.size()-2]->asGroup();
                    if (!parent)
                        return;
                    const osg::Matrixd local = osg::computeLocalToWorld(osg::NodePath(start+1, path.end()));
                    osg::ref_ptr<SceneUtil::BodyPartCoverage> clip = new SceneUtil::BodyPartCoverage;
                    if (!clip->setPlane({local(0,0), local(1,0), local(2,0), local(3,0)-end}))
                        return;
                    osg::ref_ptr<osg::Group> wrapper = new osg::Group;
                    wrapper->setName("First-person wrist beneath robe cuff");
                    wrapper->addCullCallback(clip);
                    wrapper->addChild(geometry);
                    surfaces.emplace_back(parent, wrapper, geometry);
                }
            } visitor(bone, *end-1.f);
            auto node = mObjectParts[part]->getNode();
            const auto paths = node->getParentalNodePaths();
            if (paths.empty())
                continue;
            for (std::size_t i=0; i+1<paths.front().size(); ++i)
                visitor.pushOntoNodePath(paths.front()[i]);
            node->accept(visitor);
            // Modify the scene after traversal; every wrapper belongs only to this body-part instance.
            for (const auto& [parent, wrapper, geometry] : visitor.surfaces)
                parent->replaceChild(geometry, wrapper);
            if (!visitor.surfaces.empty())
                mFirstPersonWristCoverage[part] = mPartModels[ESM::PRT_Cuirass];
        }
    }

    void NpcAnimation::fitFirstPersonRobe(osg::Node& node)
    {
        RobeRigVisitor visitor;
        node.accept(visitor);
        for (auto* rig : visitor.mRigs)
        {
            if (mBodyAwareness)
            {
                // This body has no arms of its own: the viewmodel draws them, and left here the
                // sleeves would hang from unseen arm bones in front of the camera.
                rig->removeTrianglesOfBones({ "Bip01 L UpperArm", "Bip01 R UpperArm", "Bip01 L Forearm",
                    "Bip01 R Forearm", "Bip01 L Hand", "Bip01 R Hand" });
            }
            else
            {
                // Wide hanging sleeves fill the view when the arms come up near the camera, as they do with
                // daggers. Fit them to the arm, tighter toward the hand, which stays closest to the eye.
                rig->slimAroundBones({ { "Bip01 L UpperArm", .75f }, { "Bip01 R UpperArm", .75f },
                    { "Bip01 L Forearm", .5f }, { "Bip01 R Forearm", .5f },
                    { "Bip01 L Hand", .5f }, { "Bip01 R Hand", .5f } });
            }
        }
        if (!mBodyAwareness)
            // Looking along a sleeve shows its inside: cloth, not the ground through it.
            node.getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    }

    bool NpcAnimation::addOrReplaceIndividualPart(ESM::PartReferenceType type, int group, int priority,
        VFS::Path::NormalizedView mesh, bool enchantedGlow, osg::Vec4f* glowColor, bool isLight)
    {
        if (mBodyAwareness)
        {
            // Keep the actual worn torso/leg records. Arms, head and carried equipment
            // belong exclusively to the primary viewmodel, including during a kick.
            switch (type)
            {
                case ESM::PRT_Cuirass: case ESM::PRT_Groin: case ESM::PRT_Skirt:
                case ESM::PRT_RLeg: case ESM::PRT_LLeg: case ESM::PRT_RKnee: case ESM::PRT_LKnee:
                case ESM::PRT_RAnkle: case ESM::PRT_LAnkle: case ESM::PRT_RFoot: case ESM::PRT_LFoot:
                case ESM::PRT_Tail:
                    break;
                default:
                    return false;
            }
        }
        // Hide only the camera's enclosing geometry; torso and equipment keep their actual race/body records.
        if (Settings::game().mFirstPersonFullBody && mPtr == MWMechanics::getPlayer()
            && (type == ESM::PRT_Head || type == ESM::PRT_Hair || type == ESM::PRT_Neck))
            return false;
        if ((Settings::game().mFirstPersonFullBody || MWMechanics::brutalDmFirstPerson())
            && mPtr == MWMechanics::getPlayer() && (type == ESM::PRT_LPauldron || type == ESM::PRT_RPauldron))
            return false;
        if (priority <= mPartPriorities[type])
            return false;

        removeIndividualPart(type);
        mPartslots[type] = group;
        mPartPriorities[type] = priority;
        try
        {
            std::string_view bonename = sPartList.at(type);
            if (type == ESM::PRT_Shield && carriesLeftWeapon())
                bonename = sBrutalLeftWeaponBone;
            if (type == ESM::PRT_Weapon)
            {
                const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
                MWWorld::ConstContainerStoreIterator weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
                if (weapon != inv.end() && weapon->getType() == ESM::Weapon::sRecordId)
                {
                    int weaponType = weapon->get<ESM::Weapon>()->mBase->mData.mType;
                    const std::string& weaponBonename = MWMechanics::getWeaponType(weaponType)->mAttachBone;

                    if (weaponBonename != bonename)
                    {
                        const NodeMap& nodeMap = getNodeMap();
                        NodeMap::const_iterator found = nodeMap.find(weaponBonename);
                        if (found != nodeMap.end())
                            bonename = weaponBonename;
                    }
                }
            }

            // PRT_Hair seems to be the only type that breaks consistency and uses a filter that's different from the
            // attachment bone
            const std::string_view bonefilter = (type == ESM::PRT_Hair) ? std::string_view{ "hair" } : bonename;
            mObjectParts[type] = insertBoundedPart(mesh, bonename, bonefilter, enchantedGlow, glowColor, isLight);
            mPartModels[type] = mesh.value();
            if (group == MWWorld::InventoryStore::Slot_Robe && mPtr == MWMechanics::getPlayer()
                && (mBodyAwareness || (mViewMode == VM_FirstPerson && !useFullBodyFirstPerson())))
                fitFirstPersonRobe(*mObjectParts[type]->getNode());
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "Error adding NPC part: " << e.what();
            return false;
        }

        if (!mSoundsDisabled && group == MWWorld::InventoryStore::Slot_CarriedLeft)
        {
            const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
            MWWorld::ConstContainerStoreIterator csi = inv.getSlot(group);
            if (csi != inv.end())
            {
                const auto soundId = csi->getClass().getSound(*csi);
                if (!soundId.empty())
                {
                    mSounds[type] = MWBase::Environment::get().getSoundManager()->playSound3D(
                        mPtr, soundId, 1.0f, 1.0f, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
                }
            }
        }

        osg::Node* node = mObjectParts[type]->getNode();
        if (node->getNumChildrenRequiringUpdateTraversal() > 0)
        {
            std::shared_ptr<SceneUtil::ControllerSource> src;
            if (type == ESM::PRT_Head)
            {
                src = mHeadAnimationTime;

                if (node->getUserDataContainer())
                {
                    for (unsigned int i = 0; i < node->getUserDataContainer()->getNumUserObjects(); ++i)
                    {
                        osg::Object* obj = node->getUserDataContainer()->getUserObject(i);
                        if (SceneUtil::TextKeyMapHolder* keys = dynamic_cast<SceneUtil::TextKeyMapHolder*>(obj))
                        {
                            for (const auto& key : keys->mTextKeys)
                            {
                                if (Misc::StringUtils::ciEqual(key.second, "talk: start"))
                                    mHeadAnimationTime->setTalkStart(key.first);
                                if (Misc::StringUtils::ciEqual(key.second, "talk: stop"))
                                    mHeadAnimationTime->setTalkStop(key.first);
                                if (Misc::StringUtils::ciEqual(key.second, "blink: start"))
                                    mHeadAnimationTime->setBlinkStart(key.first);
                                if (Misc::StringUtils::ciEqual(key.second, "blink: stop"))
                                    mHeadAnimationTime->setBlinkStop(key.first);
                            }

                            break;
                        }
                    }
                }
                SceneUtil::ForceControllerSourcesVisitor assignVisitor(std::move(src));
                node->accept(assignVisitor);
            }
            else
            {
                if (type == ESM::PRT_Weapon)
                    src = mWeaponAnimationTime;
                else
                    src = mAnimationTimePtr[0];
                SceneUtil::AssignControllerSourcesVisitor assignVisitor(std::move(src));
                node->accept(assignVisitor);
            }
        }

        return true;
    }

    void NpcAnimation::addPartGroup(int group, int priority, const std::vector<ESM::PartReference>& parts,
        bool enchantedGlow, osg::Vec4f* glowColor)
    {
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const MWWorld::Store<ESM::BodyPart>& partStore = store.get<ESM::BodyPart>();

        const char* ext = (mViewMode == VM_FirstPerson && !useFullBodyFirstPerson()) ? ".1st" : "";
        for (const ESM::PartReference& part : parts)
        {
            const ESM::BodyPart* bodypart = nullptr;
            if (!mNpc->isMale() && !part.mFemale.empty())
            {
                bodypart = partStore.search(ESM::RefId::stringRefId(part.mFemale.getRefIdString() + ext));
                if (!bodypart && mViewMode == VM_FirstPerson)
                {
                    bodypart = partStore.search(part.mFemale);
                    if (bodypart
                        && !(bodypart->mData.mPart == ESM::BodyPart::MP_Hand
                            || bodypart->mData.mPart == ESM::BodyPart::MP_Wrist
                            || bodypart->mData.mPart == ESM::BodyPart::MP_Forearm
                            || bodypart->mData.mPart == ESM::BodyPart::MP_Upperarm))
                        bodypart = nullptr;
                }
                else if (!bodypart)
                    Log(Debug::Warning) << "Warning: Failed to find body part '" << part.mFemale << "'";
            }
            if (!bodypart && !part.mMale.empty())
            {
                bodypart = partStore.search(ESM::RefId::stringRefId(part.mMale.getRefIdString() + ext));
                if (!bodypart && mViewMode == VM_FirstPerson)
                {
                    bodypart = partStore.search(part.mMale);
                    if (bodypart
                        && !(bodypart->mData.mPart == ESM::BodyPart::MP_Hand
                            || bodypart->mData.mPart == ESM::BodyPart::MP_Wrist
                            || bodypart->mData.mPart == ESM::BodyPart::MP_Forearm
                            || bodypart->mData.mPart == ESM::BodyPart::MP_Upperarm))
                        bodypart = nullptr;
                }
                else if (!bodypart)
                    Log(Debug::Warning) << "Warning: Failed to find body part '" << part.mMale << "'";
            }

            if (bodypart)
                addOrReplaceIndividualPart(static_cast<ESM::PartReferenceType>(part.mPart), group, priority,
                    resolvePlayerEquipmentMesh(
                        Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(bodypart->mModel))), enchantedGlow,
                    glowColor);
            else
                reserveIndividualPart((ESM::PartReferenceType)part.mPart, group, priority);
        }
    }

    void NpcAnimation::addControllers()
    {
        Animation::addControllers();

        // This world-space body never aims its limbs with the camera pitch.
        if (mBodyAwareness)
            return;
        mFirstPersonNeckController = nullptr;
        WeaponAnimation::deleteControllers();

        if (mViewMode == VM_FirstPerson)
        {
            // If there is no active animation, then the bip01 neck node will not be updated each frame, and the
            // RotateController will accumulate rotations.
            if (mStates.size() > 0)
            {
                NodeMap::iterator found = mNodeMap.find("bip01 neck");
                if (found != mNodeMap.end())
                {
                    osg::MatrixTransform* node = found->second.get();
                    mFirstPersonNeckController = new RotateController(mObjectRoot.get());
                    if (MWMechanics::brutalDmFirstPerson())
                        mFirstPersonNeckController->setRotationCarrier(
                            dynamic_cast<const osg::MatrixTransform*>(getNode("Brutal Camera Carrier")));
                    node->addUpdateCallback(mFirstPersonNeckController);
                    mActiveControllers.emplace_back(node, mFirstPersonNeckController);
                }
            }
        }
        else if (mViewMode == VM_Normal)
        {
            WeaponAnimation::addControllers(mNodeMap, mActiveControllers, mObjectRoot.get());
        }
    }

    void NpcAnimation::showWeapons(bool showWeapon)
    {
        if (mBodyAwareness)
            return; // In particular, do not clear the real player's attacking flag.
        mShowWeapons = showWeapon;
        mAmmunition.reset();
        if (showWeapon)
        {
            const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
            MWWorld::ConstContainerStoreIterator weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
            if (weapon != inv.end())
            {
                osg::Vec4f glowColor = weapon->getClass().getEnchantmentColor(*weapon);
                const VFS::Path::Normalized mesh = weapon->getClass().getCorrectedModel(*weapon);
                addOrReplaceIndividualPart(ESM::PRT_Weapon, MWWorld::InventoryStore::Slot_CarriedRight, 1, mesh,
                    !weapon->getClass().getEnchantment(*weapon).empty(), &glowColor);

                // Crossbows start out with a bolt attached
                if (weapon->getType() == ESM::Weapon::sRecordId
                    && weapon->get<ESM::Weapon>()->mBase->mData.mType == ESM::Weapon::MarksmanCrossbow)
                {
                    int ammotype = MWMechanics::getWeaponType(ESM::Weapon::MarksmanCrossbow)->mAmmoType;
                    MWWorld::ConstContainerStoreIterator ammo = inv.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
                    if (ammo != inv.end() && ammo->get<ESM::Weapon>()->mBase->mData.mType == ammotype)
                        attachArrow();
                }
            }
        }
        else
        {
            removeIndividualPart(ESM::PRT_Weapon);
            // If we remove/hide weapon from player, we should reset attack animation as well
            if (mPtr == MWMechanics::getPlayer())
                mPtr.getClass().getCreatureStats(mPtr).setAttackingOrSpell(false);
        }

        updateHolsteredWeapon(!mShowWeapons);
        updateQuiver();
    }

    bool NpcAnimation::updateCarriedLeftVisible(const int weaptype) const
    {
        if (carriesLeftWeapon())
            return mPtr.getClass().getCreatureStats(mPtr).getDrawState() == MWMechanics::DrawState::Weapon;

        // First-person armor shields are part of the drawn combat stance. A torch
        // remains useful while unarmed, and other actors keep the normal rules.
        if (mViewMode == VM_FirstPerson && MWMechanics::brutalDmFirstPerson()
            && mPtr == MWMechanics::getPlayer() && carriesShield()
            && mPtr.getClass().getCreatureStats(mPtr).getDrawState() == MWMechanics::DrawState::Nothing)
            return false;

        if (Settings::game().mShieldSheathing)
        {
            const MWWorld::Class& cls = mPtr.getClass();
            MWMechanics::CreatureStats& stats = cls.getCreatureStats(mPtr);
            if (stats.getDrawState() == MWMechanics::DrawState::Nothing)
            {
                SceneUtil::FindByNameVisitor findVisitor("Bip01 AttachShield");
                mObjectRoot->accept(findVisitor);
                if (findVisitor.mFoundNode || mViewMode == VM_FirstPerson)
                {
                    const MWWorld::InventoryStore& inv = cls.getInventoryStore(mPtr);
                    const MWWorld::ConstContainerStoreIterator shield
                        = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
                    if (shield != inv.end() && shield->getType() == ESM::Armor::sRecordId
                        && !getSheathedShieldMesh(*shield).empty())
                        return false;
                }
            }
        }

        return !(MWMechanics::getWeaponType(weaptype)->mFlags & ESM::WeaponType::TwoHanded);
    }

    void NpcAnimation::showCarriedLeft(bool show)
    {
        if (mBodyAwareness)
            return;
        if (show && mViewMode == VM_FirstPerson && MWMechanics::brutalDmFirstPerson()
            && mPtr == MWMechanics::getPlayer() && carriesShield()
            && mPtr.getClass().getCreatureStats(mPtr).getDrawState() == MWMechanics::DrawState::Nothing)
            show = false;
        // Only the Dark Messiah arms rig has a mount for a second dagger.
        if (show && carriesLeftWeapon()
            && (getNode(sBrutalLeftWeaponBone) == nullptr
                || mPtr.getClass().getCreatureStats(mPtr).getDrawState() != MWMechanics::DrawState::Weapon))
            show = false;
        mShowCarriedLeft = show;
        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        MWWorld::ConstContainerStoreIterator iter = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
        if (show && iter != inv.end())
        {
            osg::Vec4f glowColor = iter->getClass().getEnchantmentColor(*iter);
            VFS::Path::Normalized mesh = iter->getClass().getCorrectedModel(*iter);
            // For shields we must try to use the body part model
            if (iter->getType() == ESM::Armor::sRecordId)
            {
                mesh = getShieldMesh(*iter, !mNpc->isMale());
            }
            if (mesh.empty()
                || addOrReplaceIndividualPart(ESM::PRT_Shield, MWWorld::InventoryStore::Slot_CarriedLeft, 1, mesh,
                    !iter->getClass().getEnchantment(*iter).empty(), &glowColor,
                    iter->getType() == ESM::Light::sRecordId))
            {
                if (mesh.empty())
                    reserveIndividualPart(ESM::PRT_Shield, MWWorld::InventoryStore::Slot_CarriedLeft, 1);
                if (iter->getType() == ESM::Light::sRecordId && mObjectParts[ESM::PRT_Shield])
                    addExtraLight(mObjectParts[ESM::PRT_Shield]->getNode()->asGroup(),
                        SceneUtil::LightCommon(*iter->get<ESM::Light>()->mBase));
            }
        }
        else
            removeIndividualPart(ESM::PRT_Shield);

        updateHolsteredShield(mShowCarriedLeft);
    }

    void NpcAnimation::attachArrow()
    {
        WeaponAnimation::attachArrow(mPtr);

        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        MWWorld::ConstContainerStoreIterator ammo = inv.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
        if (ammo != inv.end() && !ammo->getClass().getEnchantment(*ammo).empty())
        {
            osg::Group* bone = getArrowBone();
            if (bone != nullptr && bone->getNumChildren())
                SceneUtil::addEnchantedGlow(
                    bone->getChild(0), mResourceSystem, ammo->getClass().getEnchantmentColor(*ammo));
        }

        updateQuiver();
    }

    void NpcAnimation::detachArrow()
    {
        WeaponAnimation::detachArrow(mPtr);
        updateQuiver();
    }

    void NpcAnimation::releaseArrow(float attackStrength)
    {
        WeaponAnimation::releaseArrow(mPtr, attackStrength);
        updateQuiver();
    }

    osg::Group* NpcAnimation::getArrowBone()
    {
        const PartHolder* const part = mObjectParts[ESM::PRT_Weapon].get();
        if (part == nullptr)
            return nullptr;

        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        MWWorld::ConstContainerStoreIterator weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        if (weapon == inv.end() || weapon->getType() != ESM::Weapon::sRecordId)
            return nullptr;

        int type = weapon->get<ESM::Weapon>()->mBase->mData.mType;
        int ammoType = MWMechanics::getWeaponType(type)->mAmmoType;
        if (ammoType == ESM::Weapon::None)
            return nullptr;

        // Try to find and attachment bone in actor's skeleton, otherwise fall back to the ArrowBone in weapon's mesh
        osg::Group* bone = getBoneByName(MWMechanics::getWeaponType(ammoType)->mAttachBone);
        if (bone == nullptr)
        {
            SceneUtil::FindByNameVisitor findVisitor("ArrowBone");
            part->getNode()->accept(findVisitor);
            bone = findVisitor.mFoundNode;
        }
        return bone;
    }

    osg::Node* NpcAnimation::getWeaponNode()
    {
        const PartHolder* const part = mObjectParts[ESM::PRT_Weapon].get();
        if (part == nullptr)
            return nullptr;
        return part->getNode();
    }

    Resource::ResourceSystem* NpcAnimation::getResourceSystem()
    {
        return mResourceSystem;
    }

    void NpcAnimation::enableHeadAnimation(bool enable)
    {
        mHeadAnimationTime->setEnabled(enable);
    }

    void NpcAnimation::setWeaponGroup(const std::string& group, bool relativeDuration)
    {
        mWeaponAnimationTime->setGroup(group, relativeDuration);
    }

    void NpcAnimation::equipmentChanged()
    {
        if (Settings::game().mShieldSheathing)
        {
            int weaptype = ESM::Weapon::None;
            MWMechanics::getActiveWeapon(mPtr, &weaptype);
            showCarriedLeft(updateCarriedLeftVisible(weaptype));
        }

        updateParts();
    }

    void NpcAnimation::setVampire(bool vampire)
    {
        if (mNpcType == Type_Werewolf) // we can't have werewolf vampires, can we
            return;
        if ((mNpcType == Type_Vampire) != vampire)
        {
            if (mPtr == MWMechanics::getPlayer())
                MWBase::Environment::get().getWorld()->reattachPlayerCamera();
            else
                rebuild();
        }
    }

    void NpcAnimation::setFirstPersonOffset(const osg::Vec3f& offset)
    {
        mFirstPersonOffset = offset;
    }

    void NpcAnimation::updatePtr(const MWWorld::Ptr& updated)
    {
        Animation::updatePtr(updated);
        mHeadAnimationTime->updatePtr(updated);
        if (mBodyAwarenessAnimation)
            mBodyAwarenessAnimation->updatePtr(updated);
    }

    // Remember body parts so we only have to search through the store once for each race/gender/viewmode combination
    typedef std::map<std::pair<ESM::RefId, int>, std::vector<const ESM::BodyPart*>> RaceMapping;
    static RaceMapping sRaceMapping;

    const std::vector<const ESM::BodyPart*>& NpcAnimation::getBodyParts(
        const ESM::RefId& race, bool female, bool firstPerson, bool werewolf)
    {
        constexpr int flagFirstPerson = 1 << 1;
        constexpr int flagFemale = 1 << 0;

        int flags = (werewolf ? -1 : 0);
        if (female)
            flags |= flagFemale;
        if (firstPerson)
            flags |= flagFirstPerson;

        RaceMapping::iterator found = sRaceMapping.find(std::make_pair(race, flags));
        if (found != sRaceMapping.end())
            return found->second;
        else
        {
            std::vector<const ESM::BodyPart*>& parts = sRaceMapping[std::make_pair(race, flags)];

            typedef std::multimap<ESM::BodyPart::MeshPart, ESM::PartReferenceType> BodyPartMapType;
            static const BodyPartMapType sBodyPartMap = { { ESM::BodyPart::MP_Neck, ESM::PRT_Neck },
                { ESM::BodyPart::MP_Chest, ESM::PRT_Cuirass }, { ESM::BodyPart::MP_Groin, ESM::PRT_Groin },
                { ESM::BodyPart::MP_Hand, ESM::PRT_RHand }, { ESM::BodyPart::MP_Hand, ESM::PRT_LHand },
                { ESM::BodyPart::MP_Wrist, ESM::PRT_RWrist }, { ESM::BodyPart::MP_Wrist, ESM::PRT_LWrist },
                { ESM::BodyPart::MP_Forearm, ESM::PRT_RForearm }, { ESM::BodyPart::MP_Forearm, ESM::PRT_LForearm },
                { ESM::BodyPart::MP_Upperarm, ESM::PRT_RUpperarm }, { ESM::BodyPart::MP_Upperarm, ESM::PRT_LUpperarm },
                { ESM::BodyPart::MP_Foot, ESM::PRT_RFoot }, { ESM::BodyPart::MP_Foot, ESM::PRT_LFoot },
                { ESM::BodyPart::MP_Ankle, ESM::PRT_RAnkle }, { ESM::BodyPart::MP_Ankle, ESM::PRT_LAnkle },
                { ESM::BodyPart::MP_Knee, ESM::PRT_RKnee }, { ESM::BodyPart::MP_Knee, ESM::PRT_LKnee },
                { ESM::BodyPart::MP_Upperleg, ESM::PRT_RLeg }, { ESM::BodyPart::MP_Upperleg, ESM::PRT_LLeg },
                { ESM::BodyPart::MP_Tail, ESM::PRT_Tail } };

            parts.resize(ESM::PRT_Count, nullptr);

            if (werewolf)
                return parts;

            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();

            for (const ESM::BodyPart& bodypart : store.get<ESM::BodyPart>())
            {
                if (bodypart.mData.mFlags & ESM::BodyPart::BPF_NotPlayable)
                    continue;
                if (bodypart.mData.mType != ESM::BodyPart::MT_Skin)
                    continue;

                if (!(bodypart.mRace == race))
                    continue;

                const bool partFirstPerson = ESM::isFirstPersonBodyPart(bodypart);

                bool isHand = bodypart.mData.mPart == ESM::BodyPart::MP_Hand
                    || bodypart.mData.mPart == ESM::BodyPart::MP_Wrist
                    || bodypart.mData.mPart == ESM::BodyPart::MP_Forearm
                    || bodypart.mData.mPart == ESM::BodyPart::MP_Upperarm;

                bool isSameGender = isFemalePart(&bodypart) == female;

                /* A fallback for the arms if 1st person is missing:
                 1. Try to use 3d person skin for same gender
                 2. Try to use 1st person skin for male, if female == true
                 3. Try to use 3d person skin for male, if female == true

                 A fallback in another cases: allow to use male bodyparts, if female == true
                */
                if (firstPerson && isHand && !partFirstPerson)
                {
                    // Allow 3rd person skins as a fallback for the arms if 1st person is missing
                    BodyPartMapType::const_iterator bIt
                        = sBodyPartMap.lower_bound(BodyPartMapType::key_type(bodypart.mData.mPart));
                    while (bIt != sBodyPartMap.end() && bIt->first == bodypart.mData.mPart)
                    {
                        // If we have no fallback bodypart now and bodypart is for same gender (1)
                        if (!parts[bIt->second] && isSameGender)
                            parts[bIt->second] = &bodypart;

                        // If we have fallback bodypart for other gender and found fallback for current gender (1)
                        else if (isSameGender && isFemalePart(parts[bIt->second]) != female)
                            parts[bIt->second] = &bodypart;

                        // If we have no fallback bodypart and searching for female bodyparts (3)
                        else if (!parts[bIt->second] && female)
                            parts[bIt->second] = &bodypart;

                        ++bIt;
                    }

                    continue;
                }

                // Don't allow to use podyparts for a different view
                if (partFirstPerson != firstPerson)
                    continue;

                if (female && !isFemalePart(&bodypart))
                {
                    // Allow male parts as fallback for females if female parts are missing
                    BodyPartMapType::const_iterator bIt
                        = sBodyPartMap.lower_bound(BodyPartMapType::key_type(bodypart.mData.mPart));
                    while (bIt != sBodyPartMap.end() && bIt->first == bodypart.mData.mPart)
                    {
                        // If we have no fallback bodypart now
                        if (!parts[bIt->second])
                            parts[bIt->second] = &bodypart;

                        // If we have 3d person fallback bodypart for hand and 1st person fallback found (2)
                        else if (isHand && !ESM::isFirstPersonBodyPart(*parts[bIt->second]) && partFirstPerson)
                            parts[bIt->second] = &bodypart;

                        ++bIt;
                    }

                    continue;
                }

                // Don't allow to use podyparts for another gender
                if (female != isFemalePart(&bodypart))
                    continue;

                // Use properly found bodypart, replacing fallbacks
                BodyPartMapType::const_iterator bIt
                    = sBodyPartMap.lower_bound(BodyPartMapType::key_type(bodypart.mData.mPart));
                while (bIt != sBodyPartMap.end() && bIt->first == bodypart.mData.mPart)
                {
                    parts[bIt->second] = &bodypart;
                    ++bIt;
                }
            }
            return parts;
        }
    }

    void NpcAnimation::setAccurateAiming(bool enabled)
    {
        mAccurateAiming = enabled;
    }

    bool NpcAnimation::isArrowAttached() const
    {
        return mAmmunition != nullptr;
    }

}
