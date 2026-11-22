#include "npcblood.hpp"
#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwmechanics/actorutil.hpp"
#include "../mwworld/class.hpp"
#include "decapitation.hpp"
#include "bisection.hpp"
#include "limbsevering.hpp"
#include "npcanimation.hpp"
#include "releasedequipmentcapture.hpp"
#include "renderprewarm.hpp"
#include "vismask.hpp"
#include <chrono>
#include <components/debug/debuglog.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/decalprojector.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <cstring>
#include <limits>
#include <map>
#include <osg/BlendFunc>
#include <osg/ColorMask>
#include <osg/FrameStamp>
#include <osg/PolygonOffset>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/TriangleIndexFunctor>
#include <osgUtil/CullVisitor>
#include <osgUtil/IncrementalCompileOperation>

namespace MWRender
{
    namespace
    {
        using Anchors = std::vector<SceneUtil::SurfaceDecals::Anchor>;
        constexpr std::size_t MaxWounds = 24, MaxActorVertices = 8192, MaxSaveBytes = 400000;
        const char* const Styles[]{ "slash", "small", "big", "tiny" };

        // Ordinary Geometry is often shared between actors. Replace only this
        // actor's parent edge, keeping source arrays/materials immutable.
        class BloodMarkedGeometry final : public osg::Geometry
        {
        public:
            BloodMarkedGeometry() = default;
            explicit BloodMarkedGeometry(const osg::Geometry& source)
                : osg::Geometry(source, osg::CopyOp::SHALLOW_COPY)
            {
            }
            BloodMarkedGeometry(const BloodMarkedGeometry& source, const osg::CopyOp& op)
                : osg::Geometry(source, op)
            {
            }
            META_Object(MWRender, BloodMarkedGeometry)
            osg::ref_ptr<SceneUtil::SurfaceDecals> decals;
            void accept(osg::NodeVisitor& visitor) override
            {
                if (!visitor.validNodeMask(*this))
                    return;
                osg::Geometry::accept(visitor);
                if (decals)
                    if (auto* cull = dynamic_cast<osgUtil::CullVisitor*>(&visitor))
                    {
                        visitor.pushOntoNodePath(this);
                        decals->cull(*this, *cull);
                        visitor.popFromNodePath();
                    }
            }
        };

        struct Triangles
        {
            std::vector<SceneUtil::DecalProjector::Triangle> triangles;
            void operator()(unsigned a, unsigned b, unsigned c) { triangles.push_back({ a, b, c }); }
        };
        struct Surface
        {
            int part = 0;
            unsigned ordinal = 0;
            std::string model;
            osg::ref_ptr<osg::Drawable> drawable;
            osg::ref_ptr<osg::Geometry> source;
            osg::NodePath path;
            std::vector<SceneUtil::DecalProjector::Triangle> triangles;
            std::uint64_t fingerprint = 0;

            SceneUtil::SurfaceDecals* marks() const
            {
                if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(drawable.get()))
                    return rig->getSurfaceDecals();
                if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(drawable.get()))
                    return morph->getSurfaceDecals();
                if (auto* plain = dynamic_cast<BloodMarkedGeometry*>(drawable.get()))
                    return plain->decals;
                return nullptr;
            }
            SceneUtil::SurfaceDecals* attach()
            {
                if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(drawable.get()))
                    return rig->getOrCreateSurfaceDecals();
                if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(drawable.get()))
                    return morph->getOrCreateSurfaceDecals();
                if (auto* plain = dynamic_cast<BloodMarkedGeometry*>(drawable.get()))
                {
                    if (!plain->decals)
                        plain->decals = new SceneUtil::SurfaceDecals;
                    return plain->decals;
                }
                if (path.size() < 2 || !path[path.size() - 2]->asGroup())
                    return nullptr;
                auto* plain = drawable->asGeometry();
                if (!plain)
                    return nullptr;
                osg::ref_ptr<BloodMarkedGeometry> replacement = new BloodMarkedGeometry(*plain);
                replacement->decals = new SceneUtil::SurfaceDecals;
                auto* parent = path[path.size() - 2]->asGroup();
                if (!parent->replaceChild(drawable, replacement))
                    return nullptr;
                drawable = replacement;
                source = replacement;
                path.back() = replacement;
                return replacement->decals;
            }
        };

        struct SurfacesVisitor final : osg::NodeVisitor
        {
            std::vector<Surface>& out;
            int part;
            std::string model;
            unsigned ordinal = 0;
            SurfacesVisitor(std::vector<Surface>& output, int p, std::string m)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , out(output)
                , part(p)
                , model(std::move(m))
            {
            }
            void collect(osg::Node* root, osg::Node* actor)
            {
                const auto prefix = releasedEquipmentPath(root, actor);
                for (std::size_t i = 0; i + 1 < prefix.size(); ++i)
                    pushOntoNodePath(prefix[i]);
                root->accept(*this);
                for (std::size_t i = 0; i + 1 < prefix.size(); ++i)
                    popFromNodePath();
            }
            void apply(osg::Drawable& drawable) override
            {
                Surface s;
                s.part = part;
                s.model = model;
                s.ordinal = ordinal++;
                s.drawable = &drawable;
                s.path = getNodePath();
                if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                    s.source = rig->getSourceGeometry();
                else if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                    s.source = morph->getSourceGeometry();
                else if (typeid(drawable) == typeid(osg::Geometry) || dynamic_cast<BloodMarkedGeometry*>(&drawable))
                    s.source = drawable.asGeometry();
                if (!s.source)
                    return;
                const auto* vertices = dynamic_cast<const osg::Vec3Array*>(s.source->getVertexArray());
                if (!vertices || vertices->empty() || vertices->size() > 65535)
                    return;
                osg::TriangleIndexFunctor<Triangles> functor;
                s.source->accept(functor);
                s.triangles = std::move(functor.triangles);
                std::uint64_t hash = 14695981039346656037ull;
                const auto bytes = [&](const void* data, std::size_t size) {
                    auto* p = static_cast<const unsigned char*>(data);
                    while (size--)
                    {
                        hash ^= *p++;
                        hash *= 1099511628211ull;
                    }
                };
                for (const auto& p : *vertices)
                    for (unsigned i = 0; i < 3; ++i)
                    {
                        const float value = p[i];
                        bytes(&value, sizeof(value));
                    }
                for (const auto& t : s.triangles)
                    for (auto i : t)
                        bytes(&i, sizeof(i));
                s.fingerprint = hash;
                out.push_back(std::move(s));
            }
        };

        osg::ref_ptr<osg::Geometry> posed(
            const Surface& s, const std::function<std::optional<osg::Matrixf>(std::string_view)>& bones)
        {
            if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(s.drawable.get()))
                return rig->snapshotWorldGeometry(s.path, bones, true);
            const auto* source = dynamic_cast<const osg::Vec3Array*>(s.source->getVertexArray());
            if (!source)
                return {};
            osg::ref_ptr<osg::Geometry> result = new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array(*source);
            if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(s.drawable.get()))
            {
                const auto& targets = morph->getMorphTargetList();
                if (!targets.empty())
                {
                    if (targets[0].getOffsets()->size() != vertices->size())
                        return {};
                    *vertices = *targets[0].getOffsets();
                    for (std::size_t i = 1; i < targets.size(); ++i)
                    {
                        if (targets[i].getOffsets()->size() != vertices->size())
                            return {};
                        for (std::size_t j = 0; j < vertices->size(); ++j)
                            (*vertices)[j] += (*targets[i].getOffsets())[j] * targets[i].getWeight();
                    }
                }
            }
            const auto matrix = osg::computeLocalToWorld(s.path);
            for (auto& p : *vertices)
                p = p * matrix;
            result->setVertexArray(vertices);
            return result;
        }

        struct Writer
        {
            std::string data = "BMD1";
            void u32(std::uint32_t n)
            {
                for (unsigned i = 0; i < 4; ++i)
                    data.push_back(char((n >> (i * 8)) & 255));
            }
            void f32(float f)
            {
                std::uint32_t n;
                std::memcpy(&n, &f, 4);
                u32(n);
            }
            void string(const std::string& s)
            {
                u32(s.size());
                data += s;
            }
        };
        struct Reader
        {
            std::string_view data;
            std::size_t p = 4;
            std::uint32_t u32()
            {
                if (p + 4 > data.size())
                    throw std::runtime_error("Truncated NPC blood record");
                std::uint32_t n = 0;
                for (unsigned i = 0; i < 4; ++i)
                    n |= std::uint32_t(static_cast<unsigned char>(data[p++])) << (i * 8);
                return n;
            }
            float f32()
            {
                auto n = u32();
                float f;
                std::memcpy(&f, &n, 4);
                if (!std::isfinite(f))
                    throw std::runtime_error("Nonfinite NPC blood record");
                return f;
            }
            std::string string()
            {
                const auto size = u32();
                if (size > 1024 || p + size > data.size())
                    throw std::runtime_error("Invalid NPC blood model");
                std::string s(data.substr(p, size));
                p += size;
                return s;
            }
        };
    }

    bool isRigidBloodSurface(const osg::Geometry* geometry)
    {
        return dynamic_cast<const BloodMarkedGeometry*>(geometry) != nullptr;
    }

    struct NpcAnimation::CutBloodFlow
    {
        struct Callback final : osg::NodeCallback
        {
            NpcAnimation* owner;
            explicit Callback(NpcAnimation* value)
                : owner(value)
            {
            }
            void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
            {
                traverse(node, visitor);
                if (visitor->getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
                    owner->emitCutBloodFlow(*visitor);
            }
        };
        osg::observer_ptr<osg::Group> root;
        osg::ref_ptr<Callback> callback;
        double age = 0, nextBurst = 0, nextCore = 0;
        unsigned lastTraversal = ~0u, bursts = 0, cores = 0;
        std::uint32_t random = 0x6f08ab31;
        bool active = true;
        bool torso = false;
        bool limbs = false;
        osg::Vec3f lastPosition;
        int draw(int lo, int hi)
        {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            return lo + int(random % unsigned(hi - lo + 1));
        }
    };

    void NpcAnimation::clearCutBloodFlow()
    {
        if (!mCutBloodFlow)
            return;
        if (mCutBloodFlow->root.valid() && mCutBloodFlow->callback)
            mCutBloodFlow->root->removeUpdateCallback(mCutBloodFlow->callback);
        mCutBloodFlow->root = nullptr;
        mCutBloodFlow->callback = nullptr;
        mCutBloodFlow->active = false;
    }

    void NpcAnimation::startCutBloodFlow()
    {
        if (!mDecapitation || !Settings::game().mNpcBloodDecals || !Settings::game().mBrutalImpactEffects)
            return;
        clearCutBloodFlow();
        mCutBloodFlow = std::make_shared<CutBloodFlow>();
        updateCutBloodFlow(0);
    }

    void NpcAnimation::startBisectionBloodFlow()
    {
        if (!mBisection || !Settings::game().mNpcBloodDecals || !Settings::game().mBrutalImpactEffects)
            return;
        clearCutBloodFlow();
        mCutBloodFlow = std::make_shared<CutBloodFlow>();
        mCutBloodFlow->torso = true;
        updateCutBloodFlow(0);
    }

    void NpcAnimation::startLimbSeveringBloodFlow()
    {
        if (!mLimbSevering || !Settings::game().mNpcBloodDecals || !Settings::game().mBrutalImpactEffects)
            return;
        clearCutBloodFlow();
        mCutBloodFlow = std::make_shared<CutBloodFlow>();
        mCutBloodFlow->limbs = true;
        updateCutBloodFlow(0);
    }

    void NpcAnimation::updateCutBloodFlow(float dt)
    {
        if (!mCutBloodFlow || !mCutBloodFlow->active)
            return;
        auto& flow = *mCutBloodFlow;
        flow.age += std::max(0.f, dt);
        if ((flow.limbs ? !mLimbSevering : flow.torso ? !mBisection : !mDecapitation) || flow.age >= 18 || !Settings::game().mBrutalImpactEffects)
        {
            clearCutBloodFlow();
            return;
        }
        if (flow.root.get() != mObjectRoot.get())
        {
            if (flow.root.valid() && flow.callback)
                flow.root->removeUpdateCallback(flow.callback);
            flow.root = mObjectRoot;
            flow.callback = new CutBloodFlow::Callback(this);
            mObjectRoot->addUpdateCallback(flow.callback);
        }
    }

    void NpcAnimation::emitCutBloodFlow(osg::NodeVisitor& visitor)
    {
        if (!mCutBloodFlow || !mCutBloodFlow->active
            || (mCutBloodFlow->limbs ? !mLimbSevering : mCutBloodFlow->torso ? !mBisection : !mDecapitation))
            return;
        auto& flow = *mCutBloodFlow;
        if (flow.lastTraversal == visitor.getTraversalNumber())
            return;
        flow.lastTraversal = visitor.getTraversalNumber();
        const auto surfaces = flow.limbs ? getLimbSeveringSurfacesForAudit()
            : flow.torso ? getBisectionSurfacesForAudit() : getDecapitationSurfacesForAudit();
        std::vector<std::pair<osg::Vec3f, osg::Vec3f>> outlets;
        if (flow.limbs)
        {
            for (const auto& cut : mLimbSevering->cuts)
            {
                const auto center = surfaces.find(cut.name + "StumpCenter"), normal = surfaces.find(cut.name + "StumpNormal");
                if (center != surfaces.end() && normal != surfaces.end()) outlets.emplace_back(center->second, normal->second);
            }
        }
        else
        {
            const auto center = surfaces.find("lowerCenter"), normal = surfaces.find("lowerNormal");
            if (center != surfaces.end() && normal != surfaces.end()) outlets.emplace_back(center->second, normal->second);
        }
        if (outlets.empty()) return;
        flow.lastPosition = outlets.front().first;
        const double clock = visitor.getFrameStamp() ? visitor.getFrameStamp()->getSimulationTime() : flow.age;
        const double wave = std::abs(std::sin(clock * .5));
        const float envelope = flow.age <= 1 ? 1.f : std::clamp(float((8 - flow.age - wave) / 8), 0.f, 1.f);
        const auto world = MWBase::Environment::get().getWorld();
        const auto spawn = [&](std::string_view kind, float amount) {
            for (const auto& [center, normal] : outlets)
                world->spawnBrutalImpact(kind, center, normal, amount, false);
        };
        // Dark Messiah's CutBlood_Start: neck-attached body flow. The Morrowind
        // cut centre already replaces DM's model-specific Neck+4.7 offset.
        if (flow.age < 8.45 && flow.age >= flow.nextBurst)
        {
            flow.nextBurst = flow.age + .06;
            ++flow.bursts;
            spawn("cutspatter", envelope);
            spawn("cutspray", envelope);
            if (flow.age < 3.45)
                spawn("cutdrop", envelope);
        }
        if (flow.age < 14.45 && flow.age >= flow.nextCore)
        {
            flow.nextCore = flow.age + .5;
            int count = std::min(8, int(27 - 2.5 * flow.age - 2 * wave));
            if (count < 1)
                count = flow.draw(-20, 0) < count ? 1 : 0;
            if (flow.draw(0, 10) > 4)
                for (int i = 0; i < count; ++i)
                {
                    const float amount = float((.55 + .125 * (flow.draw(0, 1000) / 1000.)) * (envelope + .5) * wave);
                    spawn("cutcore", amount);
                    ++flow.cores;
                }
        }
    }

    struct NpcAnimation::BloodDecals
    {
        struct Record
        {
            int part = 0;
            unsigned ordinal = 0, style = 0;
            std::string model;
            std::uint64_t fingerprint = 0;
            Anchors anchors;
        };
        std::vector<Record> records;
        std::array<osg::ref_ptr<osg::StateSet>, 4> states;
        std::unique_ptr<ClippedRenderPrewarm> warm;
        bool prepared = false;
        std::uint64_t added = 0, rejected = 0, evicted = 0;
        std::size_t attached = 0, vertices = 0;
        double lastDistance = 0, lastMilliseconds = 0;
    };

    void NpcAnimation::prewarmBloodDecals()
    {
        if (!Settings::game().mNpcBloodDecals || mPtr == MWMechanics::getPlayer() || mViewMode != VM_Normal
            || mBodyAwareness)
            return;
        if (!mBloodDecals)
            mBloodDecals = std::make_shared<BloodDecals>();
        auto& blood = *mBloodDecals;
        if (blood.warm && blood.warm->update())
            blood.warm.reset();
        if (blood.prepared)
            return;
        blood.prepared = true;
        osg::ref_ptr<osg::Group> warm = new osg::Group;
        for (unsigned i = 0; i < 4; ++i)
        {
            const VFS::Path::Normalized path(std::string("textures/brutal_dm/model_blood_") + Styles[i] + ".png");
            if (!mResourceSystem->getVFS()->exists(path))
                continue;
            osg::ref_ptr<osg::Texture2D> texture
                = new osg::Texture2D(mResourceSystem->getImageManager()->getImage(path));
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            mResourceSystem->getSceneManager()->applyFilterSettings(texture);
            osg::ref_ptr<osg::Geometry> geometry
                = osg::createTexturedQuadGeometry(osg::Vec3f(), osg::Vec3f(1, 0, 0), osg::Vec3f(0, 0, 1));
            auto* state = geometry->getOrCreateStateSet();
            const auto flags = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
            state->setTextureAttributeAndModes(0, texture, flags);
            state->setTextureAttribute(0, new SceneUtil::TextureType("diffuseMap"), flags);
            state->setTextureAttribute(0, new osg::TexMat, flags);
            state->setAttributeAndModes(new osg::BlendFunc(GL_DST_COLOR, GL_SRC_COLOR, GL_ZERO, GL_ONE), flags);
            state->setAttributeAndModes(new SceneUtil::AutoDepth(osg::Depth::LEQUAL, 0, 1, false), flags);
            const float offset = SceneUtil::AutoDepth::isReversed() ? 1.f : -1.f;
            state->setAttributeAndModes(new osg::PolygonOffset(offset, offset), flags);
            state->setMode(GL_CULL_FACE,
                osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
            state->setAttribute(new osg::ColorMask(true, true, true, false), flags);
            state->setRenderBinDetails(1, "RenderBin", osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);
            mResourceSystem->getSceneManager()->setUpNormalsRTForStateSet(state, false);
            mResourceSystem->getSceneManager()->recreateShaders(geometry, "blooddecal");
            // Override the source's program as well as its material state.
            state = geometry->getStateSet();
            if (auto* program = state->getAttribute(osg::StateAttribute::PROGRAM))
                state->setAttributeAndModes(program, flags);
            blood.states[i] = state;
            warm->addChild(geometry);
        }
        if (warm->getNumChildren())
        {
            if (auto* compile = mResourceSystem->getSceneManager()->getIncrementalCompileOperation())
                compile->add(warm);
            blood.warm = ClippedRenderPrewarm::attach(warm, mObjectRoot);
        }
    }

    void NpcAnimation::rebuildBloodDecals()
    {
        if (!mBloodDecals)
            return;
        auto& blood = *mBloodDecals;
        blood.attached = 0;
        blood.vertices = 0;
        std::vector<Surface> surfaces;
        for (int part = 0; part < ESM::PRT_Count; ++part)
            if (part != ESM::PRT_Weapon && part != ESM::PRT_Shield && mObjectParts[part])
            {
                SurfacesVisitor visitor(surfaces, part, mPartModels[part]);
                if (mDecapitation || mBisection || mLimbSevering)
                    visitor.setNodeMaskOverride(~0u);
                visitor.collect(mObjectParts[part]->getNode(), mPtr.getRefData().getBaseNode());
            }
        for (auto& s : surfaces)
            if (auto* marks = s.marks())
                marks->clear();
        if (!Settings::game().mNpcBloodDecals) return;
        for (const auto& record : blood.records)
            for (auto& s : surfaces)
                if (record.part == s.part && record.ordinal == s.ordinal && record.model == s.model
                    && record.fingerprint == s.fingerprint)
                    if (auto* marks = s.attach())
                        if (marks->addPatch(record.anchors, blood.states[record.style], Mask_Effect))
                        {
                            ++blood.attached;
                            blood.vertices += record.anchors.size();
                            break;
                        }
    }

    bool NpcAnimation::addBloodDecal(
        const osg::Vec3f& position, const osg::Vec3f& outward, std::string_view kind, bool power, int hitPart)
    {
        prewarmBloodDecals();
        if (!mBloodDecals || !Settings::game().mNpcBloodDecals || mDecapitation || mBisection || mLimbSevering)
            return false;
        auto& blood = *mBloodDecals;
        const unsigned style = kind == "slash" || kind == "chop" ? 0 : power ? 2 : 1;
        if (!blood.states[style])
            return false;
        const auto start = std::chrono::steady_clock::now();
        try
        {
            std::vector<Surface> surfaces;
            for (int part = 0; part < ESM::PRT_Count; ++part)
                if (part != ESM::PRT_Weapon && part != ESM::PRT_Shield && part != ESM::PRT_Hair && mObjectParts[part]
                    && (hitPart < 0 || hitPart == part))
                {
                    SurfacesVisitor visitor(surfaces, part, mPartModels[part]);
                    visitor.collect(mObjectParts[part]->getNode(), mPtr.getRefData().getBaseNode());
                }
            const auto bones = [this](std::string_view name) { return getBoneWorldMatrix(name); };
            SceneUtil::DecalProjector::Result best;
            Surface* selected = nullptr;
            auto tangent = osg::Vec3d(0, 0, 1) ^ osg::Vec3d(outward);
            if (kind == "chop")
                tangent = osg::Vec3d(0, 0, 1);
            // Native units: the DM slash is 64 px at .1 Source inches/px.
            const double size = (style == 0 ? 15. : 24.) * (power ? 1.35 : 1.);
            for (auto& s : surfaces)
                if (auto geometry = posed(s, bones))
                {
                    const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray());
                    if (!vertices)
                        continue;
                    std::vector<osg::Vec3d> world(vertices->begin(), vertices->end());
                    auto projection = SceneUtil::DecalProjector::project(
                        world, s.triangles, osg::Vec3d(position), osg::Vec3d(outward), tangent, size, size);
                    if (!projection.vertices.empty() && projection.distance < best.distance)
                    {
                        best = std::move(projection);
                        selected = &s;
                    }
                }
            if (!selected)
            {
                ++blood.rejected;
                return false;
            }
            BloodDecals::Record record{ selected->part, selected->ordinal, style, selected->model,
                selected->fingerprint, std::move(best.vertices) };
            std::size_t total = 0;
            for (const auto& r : blood.records)
                total += r.anchors.size();
            while (!blood.records.empty()
                && (blood.records.size() >= MaxWounds || total + record.anchors.size() > MaxActorVertices))
            {
                total -= blood.records.front().anchors.size();
                blood.records.erase(blood.records.begin());
                ++blood.evicted;
            }
            blood.records.push_back(std::move(record));
            ++blood.added;
            blood.lastDistance = best.distance;
            rebuildBloodDecals();
            blood.lastMilliseconds
                = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            return true;
        }
        catch (const std::exception& error)
        {
            ++blood.rejected;
            Log(Debug::Warning) << "NPC blood projection rejected: " << error.what();
            return false;
        }
    }

    std::string NpcAnimation::getBloodDecalData() const
    {
        if (!mBloodDecals || mBloodDecals->records.empty())
            return {};
        Writer out;
        out.u32(mBloodDecals->records.size());
        for (const auto& r : mBloodDecals->records)
        {
            out.u32(r.part);
            out.u32(r.ordinal);
            out.u32(r.style);
            out.string(r.model);
            out.u32(r.fingerprint & 0xffffffff);
            out.u32(r.fingerprint >> 32);
            out.u32(r.anchors.size());
            for (const auto& a : r.anchors)
            {
                for (auto i : a.indices)
                    out.u32(i);
                for (unsigned i = 0; i < 3; ++i)
                    out.f32(a.weights[i]);
                out.f32(a.uv.x());
                out.f32(a.uv.y());
            }
        }
        return out.data;
    }

    bool NpcAnimation::restoreBloodDecals(std::string_view data)
    {
        prewarmBloodDecals();
        if (!mBloodDecals || !Settings::game().mNpcBloodDecals)
            return false;
        try
        {
            std::vector<BloodDecals::Record> records;
            if (!data.empty())
            {
                if (data.size() > MaxSaveBytes || data.substr(0, 4) != "BMD1")
                    return false;
                Reader in{ data };
                const auto count = in.u32();
                if (count > MaxWounds)
                    return false;
                std::size_t total = 0;
                for (unsigned j = 0; j < count; ++j)
                {
                    BloodDecals::Record r;
                    r.part = in.u32();
                    r.ordinal = in.u32();
                    r.style = in.u32();
                    r.model = in.string();
                    r.fingerprint = in.u32();
                    r.fingerprint |= std::uint64_t(in.u32()) << 32;
                    const auto size = in.u32();
                    if (r.part < 0 || r.part >= ESM::PRT_Count || r.ordinal > 4096 || r.style >= 4 || !size || size % 3
                        || size > 3072 || (total += size) > MaxActorVertices)
                        return false;
                    r.anchors.reserve(size);
                    for (unsigned i = 0; i < size; ++i)
                    {
                        SceneUtil::SurfaceDecals::Anchor a;
                        for (auto& index : a.indices)
                        {
                            index = in.u32();
                            if (index > 65535)
                                return false;
                        }
                        for (unsigned k = 0; k < 3; ++k)
                            a.weights[k] = in.f32();
                        const float u = in.f32(), v = in.f32();
                        a.uv = { u, v };
                        r.anchors.push_back(a);
                    }
                    osg::ref_ptr<SceneUtil::SurfaceDecals> validate = new SceneUtil::SurfaceDecals;
                    if (!validate->addPatch(r.anchors, new osg::StateSet, Mask_Effect))
                        return false;
                    records.push_back(std::move(r));
                }
                if (in.p != data.size())
                    return false;
            }
            mBloodDecals->records = std::move(records);
            rebuildBloodDecals();
            if (mDecapitation)
                transferBloodDecalsToHead(*mDecapitation);
            if (mBisection)
            {
                transferBloodDecalsToTorso(*mBisection);
                mBisection->prepareDrawBuffers();
            }
            if (mLimbSevering)
            {
                transferBloodDecalsToLimbs(*mLimbSevering);
                mLimbSevering->prepareDrawBuffers();
            }
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    std::map<std::string, double> NpcAnimation::getBloodDecalStats() const
    {
        if (!mBloodDecals)
            return { { "enabled", Settings::game().mNpcBloodDecals ? 1. : 0. }, { "available", 0 }, { "records", 0 },
                { "attached", 0 } };
        const auto& b = *mBloodDecals;
        std::size_t frozenHead = 0, frozenNeck = 0;
        if (mDecapitation)
            for (unsigned side = 0; side < 2; ++side)
            {
                const auto* group = side ? mDecapitation->upper.get() : mDecapitation->lower.get();
                for (unsigned i = 0; i < group->getNumChildren(); ++i)
                    if (group->getChild(i)->getName() == "Frozen NPC blood decal")
                        ++(side ? frozenHead : frozenNeck);
            }
        return { { "enabled", Settings::game().mNpcBloodDecals ? 1. : 0. }, { "available", b.states[0] ? 1. : 0. },
            { "records", double(b.records.size()) }, { "attached", double(b.attached) },
            { "vertices", double(b.vertices) }, { "added", double(b.added) }, { "rejected", double(b.rejected) },
            { "evicted", double(b.evicted) }, { "lastDistance", b.lastDistance },
            { "lastMilliseconds", b.lastMilliseconds }, { "limit", double(MaxWounds) },
            { "frozenHead", double(frozenHead) }, { "frozenNeck", double(frozenNeck) },
            { "bisectedTriangles", mBisection ? double(mBisection->decalTriangles) : 0. },
            { "limbTriangles", mLimbSevering ? double(mLimbSevering->decalTriangles) : 0. },
            { "lastPart", b.records.empty() ? -1. : double(b.records.back().part) },
            { "garmentCoverage", mGarmentCoverage ? 1. : 0. },
            { "cutFlowActive", mCutBloodFlow && mCutBloodFlow->active ? 1. : 0. },
            { "cutFlowAge", mCutBloodFlow ? mCutBloodFlow->age : 0. },
            { "cutFlowBursts", mCutBloodFlow ? double(mCutBloodFlow->bursts) : 0. },
            { "cutFlowCores", mCutBloodFlow ? double(mCutBloodFlow->cores) : 0. } };
    }

    void NpcAnimation::transferBloodDecalsToTorso(BisectionGeometry& target)
    {
        target.clearDecals();
        if (!mBloodDecals || !Settings::game().mNpcBloodDecals) return;
        std::vector<Surface> surfaces;
        for (auto part : { ESM::PRT_Cuirass, ESM::PRT_Groin, ESM::PRT_Skirt })
            if (mObjectParts[part])
            {
                SurfacesVisitor visitor(surfaces, part, mPartModels[part]);
                visitor.setNodeMaskOverride(~0u);
                visitor.collect(mObjectParts[part]->getNode(), mPtr.getRefData().getBaseNode());
            }
        for (const auto& record : mBloodDecals->records)
            for (const auto& surface : surfaces)
                if (record.part == surface.part && record.ordinal == surface.ordinal && record.model == surface.model
                    && record.fingerprint == surface.fingerprint && record.style < mBloodDecals->states.size()
                    && mBloodDecals->states[record.style])
                {
                    SceneUtil::SurfaceDecals::PatchData patch{ record.anchors, mBloodDecals->states[record.style], Mask_Effect };
                    for (auto& anchor : patch.vertices)
                        anchor.weights /= anchor.weights.x() + anchor.weights.y() + anchor.weights.z();
                    target.addDecal(surface.drawable, patch);
                    break;
                }
    }

    void NpcAnimation::transferBloodDecalsToLimbs(LimbSeveringGeometry& target)
    {
        target.clearDecals();
        if (!mBloodDecals || !Settings::game().mNpcBloodDecals) return;
        std::vector<Surface> surfaces;
        for (int part = 0; part < ESM::PRT_Count; ++part)
            if (mObjectParts[part] && part != ESM::PRT_Weapon && part != ESM::PRT_Shield)
            {
                SurfacesVisitor visitor(surfaces, part, mPartModels[part]);
                visitor.setNodeMaskOverride(~0u);
                visitor.collect(mObjectParts[part]->getNode(), mPtr.getRefData().getBaseNode());
            }
        for (const auto& record : mBloodDecals->records)
            for (const auto& surface : surfaces)
                if (record.part == surface.part && record.ordinal == surface.ordinal && record.model == surface.model
                    && record.fingerprint == surface.fingerprint && record.style < mBloodDecals->states.size()
                    && mBloodDecals->states[record.style])
                {
                    SceneUtil::SurfaceDecals::PatchData patch{ record.anchors, mBloodDecals->states[record.style], Mask_Effect };
                    for (auto& anchor : patch.vertices)
                        anchor.weights /= anchor.weights.x() + anchor.weights.y() + anchor.weights.z();
                    target.addDecal(surface.drawable, patch);
                    break;
                }
    }

    void NpcAnimation::transferBloodDecalsToHead(DecapitationGeometry& target, bool bindSources)
    {
        if (bindSources)
        {
            std::vector<Surface> surfaces;
            for (auto part : { ESM::PRT_Head, ESM::PRT_Hair, ESM::PRT_Neck })
                if (mObjectParts[part])
                {
                    SurfacesVisitor visitor(surfaces, part, mPartModels[part]);
                    visitor.setNodeMaskOverride(~0u);
                    visitor.collect(mObjectParts[part]->getNode(), mPtr.getRefData().getBaseNode());
                }
            for (auto& cached : target.bloodSources)
            {
                osg::ref_ptr<osg::Drawable> drawable;
                if (cached.sourceDrawable.lock(drawable))
                    for (const auto& surface : surfaces)
                        if (surface.drawable == drawable)
                        {
                            cached.part = surface.part;
                            cached.ordinal = surface.ordinal;
                            cached.model = surface.model;
                            cached.fingerprint = surface.fingerprint;
                            break;
                        }
                cached.sourceDrawable = nullptr;
            }
        }

        // Prepare replacements before removing the old overlays. In particular,
        // clearing an empty record must still remove all previously frozen blood.
        std::array<std::vector<osg::ref_ptr<osg::Geometry>>, 2> replacements;
        if (mBloodDecals)
            for (const auto& record : mBloodDecals->records)
            {
                if (record.style >= mBloodDecals->states.size() || !mBloodDecals->states[record.style])
                    continue;
                for (const auto& cached : target.bloodSources)
                {
                    if (cached.part != record.part || cached.ordinal != record.ordinal || cached.model != record.model
                        || cached.fingerprint != record.fingerprint || !cached.positions)
                        continue;
                    // The original head may now be detached or animated elsewhere.
                    // Only the same cut-frame vertices and inverse matrices that
                    // built target.lower/upper can reconstruct its frozen surface.
                    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
                    geometry->setVertexArray(cached.positions);
                    SceneUtil::SurfaceDecals::PatchData patch{ record.anchors, mBloodDecals->states[record.style],
                        Mask_Effect };
                    // Match addPatch's canonical weights, including records read
                    // from a save rather than an existing live SurfaceDecals.
                    for (auto& anchor : patch.vertices)
                        anchor.weights /= anchor.weights.x() + anchor.weights.y() + anchor.weights.z();
                    osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array(patch.vertices.size()),
                                                 normals = new osg::Vec3Array(patch.vertices.size());
                    if (!SceneUtil::SurfaceDecals::evaluate(*geometry, patch, *positions, *normals))
                        continue;
                    std::vector<DecapitationGeometry::Vertex> low, high;
                    for (std::size_t i = 0; i < positions->size(); i += 3)
                    {
                        std::vector<DecapitationGeometry::Vertex> triangle;
                        for (unsigned j = 0; j < 3; ++j)
                            triangle.push_back({ (*positions)[i + j], (*normals)[i + j], patch.vertices[i + j].uv,
                                osg::Vec4f(1, 1, 1, 1) });
                        if (cached.wholeHead)
                            DecapitationGeometry::appendPolygon(high, triangle);
                        else
                        {
                            const auto distance = [&](const osg::Vec3f& pos) {
                                return (pos - target.diagnosticCenter) * target.diagnosticNormal;
                            };
                            DecapitationGeometry::appendPolygon(
                                high, DecapitationGeometry::clipPolygon(triangle, distance, true));
                            DecapitationGeometry::appendPolygon(
                                low, DecapitationGeometry::clipPolygon(triangle, distance, false));
                        }
                    }
                    for (unsigned side = 0; side < 2; ++side)
                    {
                        auto& vertices = side ? high : low;
                        if (vertices.empty())
                            continue;
                        auto frozen = DecapitationGeometry::makeGeometry(
                            vertices, patch.stateSet, side ? target.bloodHeadInverse : target.bloodNeckInverse);
                        frozen->setNodeMask(Mask_Effect);
                        frozen->setName("Frozen NPC blood decal");
                        replacements[side].push_back(frozen);
                    }
                    break;
                }
            }
        for (unsigned side = 0; side < 2; ++side)
        {
            auto* group = side ? target.upper.get() : target.lower.get();
            for (unsigned i = group->getNumChildren(); i-- > 0;)
                if (group->getChild(i)->getName() == "Frozen NPC blood decal")
                    group->removeChild(i);
            for (const auto& geometry : replacements[side])
                group->addChild(geometry);
        }
    }
}
