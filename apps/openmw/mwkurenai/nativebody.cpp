#include "nativebody.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <map>

#include <osg/BlendFunc>
#include <osg/Group>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Quat>

#include <components/debug/debuglog.hpp>
#include <components/misc/jigglepolicy.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <kurenai/player.hpp>

#include "../mwbase/environment.hpp"
#include "../mwrender/jigglebonecontroller.hpp"
#include "../mwbase/world.hpp"
#include "../mwrender/animation.hpp"
#include "../mwworld/ptr.hpp"

namespace MWKurenai
{
    namespace
    {
        // Cull callback on the player's Morrowind body: while the native body shows, the body is not drawn (nor its
        // shadow); its animation still runs in the update traversal, so it is ready when F6 hands her back.
        class HideBody : public SceneUtil::NodeCallback<HideBody>
        {
        public:
            std::atomic<bool> mHidden{ false };
            void operator()(osg::Node* node, osg::NodeVisitor* nv)
            {
                if (!mHidden.load(std::memory_order_relaxed))
                    traverse(node, nv);
            }
        };

        // "krn_<index>_<game name>" -> index; -1 for anything else.
        int boneIndex(const std::string& name)
        {
            if (name.size() < 7 || name.compare(0, 4, "krn_") != 0 || !std::isdigit(static_cast<unsigned char>(name[4]))
                || !std::isdigit(static_cast<unsigned char>(name[5])) || name[6] != '_')
                return -1;
            return (name[4] - '0') * 10 + (name[5] - '0');
        }

        class CollectBones : public osg::NodeVisitor
        {
        public:
            CollectBones()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }
            std::map<int, osg::MatrixTransform*> mFound;
            void apply(osg::MatrixTransform& node) override
            {
                const int i = boneIndex(node.getName());
                if (i >= 0)
                    mFound.emplace(i, &node);
                traverse(node);
            }
        };

        // Nodes named krn_..._m<index>_... (one per mesh): the game's material index -> nodes.
        class CollectMaterials : public osg::NodeVisitor
        {
        public:
            CollectMaterials()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }
            std::map<int, std::vector<osg::ref_ptr<osg::Node>>> mFound;
            void apply(osg::Node& node) override
            {
                // Shapes are named krn_<model>_m<material>_<n>_<texture>_mat: the first "_m" followed by digits and
                // then '_' or the end (the "_mat" suffix is not it).
                const std::string& n = node.getName();
                if (n.compare(0, 4, "krn_") == 0)
                {
                    for (auto at = n.find("_m"); at != std::string::npos; at = n.find("_m", at + 2))
                    {
                        std::size_t i = at + 2;
                        int index = 0;
                        while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i])))
                            index = index * 10 + (n[i++] - '0');
                        if (i > at + 2 && (i == n.size() || n[i] == '_'))
                        {
                            mFound[index].push_back(&node);
                            break;
                        }
                    }
                }
                traverse(node);
            }
        };

        bool live(const kurenai::proto::Pose& pose, int slot)
        {
            if (slot < 0 || static_cast<std::size_t>(slot) >= pose.bones.size())
                return false;
            const kurenai::proto::Vec3& t = pose.bones[slot];
            const kurenai::proto::Axes& a = pose.axes[slot];
            // The game leaves the slots of bones it doesn't animate (her face) zero: no position, unit axes.
            return (t.x != 0 || t.y != 0 || t.z != 0) && a.x.x * a.x.x + a.x.y * a.x.y + a.x.z * a.x.z > 1e-6f;
        }

        // Character space: x left, y up, z forward (game units); Morrowind actor space: x right, y forward, z up.
        const osg::Matrixf kCharToActor(-1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1);
    }

    NativeBody::NativeBody(std::vector<std::string> models)
        : mPaths(std::move(models))
    {
    }

    NativeBody::~NativeBody()
    {
        hide(false);
        osg::ref_ptr<osg::Group> parent;
        if (mPlace && mParent.lock(parent))
            parent->removeChild(mPlace);
    }

    const std::string& NativeBody::current() const
    {
        static const std::string none;
        return mShown >= 0 ? mModels[mShown].path : none;
    }

    void NativeBody::setOutfit(const std::string& path)
    {
        if (path == mWanted)
            return;
        mWanted = path;
        mFits.clear(); // fitted again for the chosen model on the next frame
    }

    std::vector<std::string> NativeBody::outfits()
    {
        load();
        std::vector<std::string> out;
        for (const Model& m : mModels)
            out.push_back(m.path);
        return out;
    }

    void NativeBody::load()
    {
        if (mLoaded)
            return;
        mLoaded = true;
        Resource::ResourceSystem* resources = MWBase::Environment::get().getResourceSystem();
        for (const std::string& path : mPaths)
        {
            const VFS::Path::Normalized p(path);
            if (p.value().empty())
                continue;
            if (!resources->getVFS()->exists(p))
            {
                Log(Debug::Info) << "Kurenai: native body " << path << " not installed";
                continue;
            }
            Model m;
            m.path = path;
            try
            {
                m.instance = resources->getSceneManager()->getInstance(p);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Error) << "Kurenai: native body " << path << ": " << e.what();
                continue;
            }
            CollectBones collect;
            m.instance->accept(collect);
            if (collect.mFound.empty())
            {
                Log(Debug::Error) << "Kurenai: native body " << path << " has no krn_ bones";
                continue;
            }
            m.bones.resize(collect.mFound.rbegin()->first + 1);
            for (const auto& [i, node] : collect.mFound)
            {
                Bone& b = m.bones[i];
                b.node = node;
                b.name = node->getName();
                b.rest = node->getMatrix();
                node->setDataVariance(osg::Object::DYNAMIC);
                for (osg::Node* up = node->getNumParents() ? node->getParent(0) : nullptr; up;
                     up = up->getNumParents() ? up->getParent(0) : nullptr)
                {
                    const int pi = boneIndex(up->getName());
                    if (pi >= 0)
                    {
                        b.parent = pi;
                        break;
                    }
                }
            }
            if (auto* skel = dynamic_cast<SceneUtil::Skeleton*>(m.instance.get()))
                skel->setActive(SceneUtil::Skeleton::Active);
            CollectMaterials materials;
            m.instance->accept(materials);
            m.materials = std::move(materials.mFound);
            // OpenMW's jiggle on her glutes and chest: the model's krn_jiggle_ bones (tools/rn/jiggle.py) get the
            // same spring as any jiggle bone, on top of the game's pose (only the game's own bones are set from
            // PCSX2), when the jiggle settings enable it for the player.
            struct Jiggle : osg::NodeVisitor
            {
                Jiggle()
                    : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                {
                }
                std::vector<osg::MatrixTransform*> mFound;
                void apply(osg::MatrixTransform& node) override
                {
                    if (node.getName().compare(0, 11, "krn_jiggle_") == 0)
                        mFound.push_back(&node);
                    traverse(node);
                }
            } jiggle;
            m.instance->accept(jiggle);
            const bool jiggleOn = Misc::JigglePolicy::actorEnabled(true, "Kurenai", ESM::FormId());
            for (osg::MatrixTransform* node : jiggle.mFound)
                if (jiggleOn)
                {
                    node->setDataVariance(osg::Object::DYNAMIC);
                    node->addUpdateCallback(new MWRender::JiggleBoneController(false, true, "Kurenai"));
                }
            if (!jiggle.mFound.empty())
                Log(Debug::Info) << "Kurenai: native body jiggle: " << jiggle.mFound.size() << " bones"
                                 << (jiggleOn ? "" : " (off in the jiggle settings)");
            Log(Debug::Info) << "Kurenai: native body " << path << ": " << collect.mFound.size() << " bones";
            mModels.push_back(std::move(m));
        }
        for (std::uint32_t slot = 0; slot < kurenai::proto::kMaxItems; ++slot)
        {
            const VFS::Path::Normalized p("meshes/krn_native/krn_item_" + std::to_string(slot) + ".nif");
            if (!resources->getVFS()->exists(p))
                continue;
            try
            {
                Item item;
                item.place = new osg::MatrixTransform;
                item.place->setName("krn_item_place_" + std::to_string(slot));
                item.place->setDataVariance(osg::Object::DYNAMIC);
                item.place->setNodeMask(0);
                osg::ref_ptr<osg::Node> node = resources->getSceneManager()->getInstance(p);
                item.place->addChild(node);
                item.versions.push_back(node);
                if (slot == 0 || slot == 2) // the blade's and the weight's other versions (her upgrade levels)
                    for (int v = 1; v <= 2; ++v)
                    {
                        const VFS::Path::Normalized pv(
                            "meshes/krn_native/krn_item_" + std::to_string(slot) + "_" + std::to_string(v) + ".nif");
                        if (!resources->getVFS()->exists(pv))
                            break;
                        osg::ref_ptr<osg::Node> extra = resources->getSceneManager()->getInstance(pv);
                        item.place->addChild(extra);
                        item.versions.push_back(extra);
                    }
                CollectMaterials materials;
                for (const osg::ref_ptr<osg::Node>& version : item.versions)
                    version->accept(materials);
                item.materials = std::move(materials.mFound);
                mItems.emplace(slot, std::move(item));
            }
            catch (const std::exception& e)
            {
                Log(Debug::Error) << "Kurenai: native item " << p.value() << ": " << e.what();
            }
        }
        if (!mItems.empty())
            Log(Debug::Info) << "Kurenai: native body: " << mItems.size() << " items";
    }

    void NativeBody::applyColors(
        std::map<int, std::vector<osg::ref_ptr<osg::Node>>>& nodes, const std::vector<kurenai::proto::Color>& colors)
    {
        // The game's material colour multiplies the texture (1 = as textured; brighter or darker when it tints her,
        // fainter when its alpha drops). Applied over the NIF's own material only while it differs from white.
        for (auto& [index, list] : nodes)
        {
            const kurenai::proto::Color c = index < static_cast<int>(colors.size()) ? colors[index] : kurenai::proto::Color{};
            const bool plain = c.r == 1 && c.g == 1 && c.b == 1 && c.a == 1;
            for (osg::ref_ptr<osg::Node>& node : list)
            {
                if (plain)
                {
                    if (osg::StateSet* ss = node->getStateSet())
                    {
                        ss->removeAttribute(osg::StateAttribute::MATERIAL);
                        ss->removeAttribute(osg::StateAttribute::BLENDFUNC);
                        ss->removeUniform("colorMode");
                        ss->setRenderingHint(osg::StateSet::DEFAULT_BIN);
                        ss->setRenderBinMode(osg::StateSet::INHERIT_RENDERBIN_DETAILS);
                    }
                    continue;
                }
                osg::StateSet* ss = node->getOrCreateStateSet();
                osg::ref_ptr<osg::Material> m = new osg::Material;
                m->setColorMode(osg::Material::OFF);
                m->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(c.r, c.g, c.b, c.a));
                m->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(c.r, c.g, c.b, c.a));
                ss->setAttributeAndModes(m, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                // The shaders take diffuse/ambient from the vertex colours when the NIF has them, which would ignore
                // this material (Kiton's fade never showed); force material colours like Animation's fade does.
                ss->addUniform(new osg::Uniform("colorMode", 0), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                if (c.a < 0.999f)
                {
                    ss->setAttributeAndModes(new osg::BlendFunc(osg::BlendFunc::SRC_ALPHA, osg::BlendFunc::ONE_MINUS_SRC_ALPHA),
                        osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                    ss->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                    ss->setRenderBinMode(osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);
                }
                else
                {
                    ss->removeAttribute(osg::StateAttribute::BLENDFUNC);
                    ss->setRenderingHint(osg::StateSet::DEFAULT_BIN);
                    ss->setRenderBinMode(osg::StateSet::INHERIT_RENDERBIN_DETAILS);
                }
            }
        }
    }

    NativeBody::Fit NativeBody::fitModel(const kurenai::proto::Pose& pose, int model, int hipSlot) const
    {
        // The game's bone array holds the model's nodes in the model's order, shifted (the stage 1 kimono's: one slot
        // later than the model file; the cutscene model's: twelve). The shift is measured, not assumed: from her hips
        // on, each rigid bone is exactly where its parent's live matrix puts its bind offset.
        const std::vector<Bone>& bones = mModels[model].bones;
        Fit f;
        f.model = model;
        f.slots.assign(bones.size(), -1);
        std::vector<bool> used(pose.bones.size(), false);
        int hip = -1;
        for (std::size_t n = 0; n < bones.size(); ++n)
            if (bones[n].node && bones[n].name.find("_bb_hip") != std::string::npos)
                hip = static_cast<int>(n);
        if (hip < 0)
            return f;
        f.slots[hip] = hipSlot;
        used[hipSlot] = true;
        const auto expected = [&](int n, int sp) {
            const kurenai::proto::Axes& a = pose.axes[sp];
            const kurenai::proto::Vec3& t = pose.bones[sp];
            const osg::Vec3f r = bones[n].rest.getTrans();
            return osg::Vec3f(r.x() * a.x.x + r.y() * a.y.x + r.z() * a.z.x + t.x,
                r.x() * a.x.y + r.y() * a.y.y + r.z() * a.z.y + t.y, r.x() * a.x.z + r.y() * a.y.z + r.z() * a.z.z + t.z);
        };
        const auto distance = [&](const osg::Vec3f& e, int s) {
            const kurenai::proto::Vec3& t = pose.bones[s];
            return (e - osg::Vec3f(t.x, t.y, t.z)).length();
        };
        // Exact matches first (rigid bones, in parent order).
        for (std::size_t n = 0; n < bones.size(); ++n)
        {
            const Bone& b = bones[n];
            if (!b.node || f.slots[n] >= 0 || b.parent < 0 || f.slots[b.parent] < 0)
                continue;
            const osg::Vec3f e = expected(static_cast<int>(n), f.slots[b.parent]);
            int pick = -1;
            float best = 0.5f;
            for (int s = 0; s < static_cast<int>(pose.bones.size()); ++s)
                if (!used[s] && live(pose, s) && distance(e, s) < best)
                {
                    best = distance(e, s);
                    pick = s;
                }
            if (pick >= 0)
            {
                f.slots[n] = pick;
                used[pick] = true;
                ++f.matched;
            }
        }
        // The rest moved by the game's cloth (sleeves, hem: up to 36 units from the bind pose) or offset by it (her
        // hips' cloth): the slot at the shift most exact matches share. The array keeps the model's order apart from
        // that shift, so this is the bone's own slot however far the cloth has swung it.
        std::map<int, int> shifts;
        for (std::size_t n = 0; n < bones.size(); ++n)
            if (f.slots[n] >= 0 && static_cast<int>(n) != hip)
                ++shifts[f.slots[n] - static_cast<int>(n)];
        if (shifts.empty())
            return f;
        const int shift = std::max_element(shifts.begin(), shifts.end(),
            [](const auto& x, const auto& y) { return x.second < y.second; })->first;
        f.shift = shift;
        for (std::size_t n = 0; n < bones.size(); ++n)
        {
            const int s = static_cast<int>(n) + shift;
            if (bones[n].node && f.slots[n] < 0 && s >= 0 && s < static_cast<int>(pose.bones.size()) && !used[s]
                && live(pose, s))
            {
                f.slots[n] = s;
                used[s] = true;
            }
        }
        // Score: bones exactly where their (now placed) parent puts them.
        f.matched = 0;
        for (std::size_t n = 0; n < bones.size(); ++n)
            if (f.slots[n] >= 0 && bones[n].parent >= 0 && f.slots[bones[n].parent] >= 0
                && distance(expected(static_cast<int>(n), f.slots[bones[n].parent]), f.slots[n]) < 0.5f)
                ++f.matched;
        return f;
    }

    NativeBody::Fit NativeBody::fit(const kurenai::proto::Pose& pose) const
    {
        Fit best;
        for (std::size_t mi = 0; mi < mModels.size(); ++mi)
        {
            if (!mWanted.empty() && mModels[mi].path != mWanted)
                continue;
            for (int h = 0; h < static_cast<int>(pose.bones.size()); ++h)
                if (live(pose, h))
                {
                    Fit f = fitModel(pose, static_cast<int>(mi), h);
                    if (f.matched > best.matched)
                        best = std::move(f);
                }
            // The first model that fits is hers (the red kimono); the others only when chosen.
            if (best.matched >= 16)
                break;
        }
        // Her body is about 28 rigid bones (legs, spine, arms, hands, head): most of them must match.
        if (best.matched < 16)
            return {};
        return best;
    }

    void NativeBody::attach(const MWWorld::Ptr& player, int model)
    {
        SceneUtil::PositionAttitudeTransform* base = player.getRefData().getBaseNode();
        if (!base)
            return;
        if (!mPlace)
        {
            mPlace = new osg::MatrixTransform;
            mPlace->setName("krn_native_place");
            mPlace->setDataVariance(osg::Object::DYNAMIC);
            mPlace->addCullCallback(new SceneUtil::LightListCallback);
        }
        osg::ref_ptr<osg::Group> parent;
        if (!mParent.lock(parent) || parent.get() != base)
        {
            if (parent)
                parent->removeChild(mPlace);
            base->addChild(mPlace);
            mParent = base;
        }
        if (mShown != model)
        {
            mPlace->removeChildren(0, mPlace->getNumChildren());
            mPlace->addChild(mModels[model].instance);
            mShown = model;
            Log(Debug::Info) << "Kurenai: native body " << mModels[model].path << " shown";
        }
        mPlace->setNodeMask(~0u);
    }

    void NativeBody::hide(bool hidden)
    {
        osg::Node* root = nullptr;
        if (hidden)
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            if (MWRender::Animation* anim = world ? world->getAnimation(world->getPlayerPtr()) : nullptr)
                root = anim->getObjectRoot();
        }
        osg::ref_ptr<osg::Node> current;
        mHiddenRoot.lock(current);
        if (hidden && root && current.get() != root)
        {
            if (current && mHideCallback)
                current->removeCullCallback(mHideCallback);
            if (!mHideCallback)
                mHideCallback = new HideBody;
            root->addCullCallback(mHideCallback);
            mHiddenRoot = root;
        }
        if (mHideCallback)
            static_cast<HideBody*>(mHideCallback.get())->mHidden = hidden;
    }

    bool NativeBody::update(const MWWorld::Ptr& player, bool show, const kurenai::proto::Pose* pose,
        const kurenai::proto::PlayerState* state, float scale, bool faint)
    {
        Fit f;
        if (show && pose && state && !pose->bones.empty() && pose->axes.size() == pose->bones.size())
        {
            load();
            auto it = mFits.find(pose->model);
            if (it == mFits.end())
            {
                it = mFits.emplace(pose->model, fit(*pose)).first;
                if (it->second.model >= 0)
                    Log(Debug::Info) << "Kurenai: model " << std::hex << pose->model << std::dec << " is native body "
                                     << mModels[it->second.model].path << " (" << it->second.matched
                                     << " bones matched exactly, slot shift " << it->second.shift << ", "
                                     << std::count_if(it->second.slots.begin(), it->second.slots.end(),
                                            [](int s) { return s >= 0; })
                                     << " live)";
                if (it->second.model >= 0)
                {
                    std::string still;
                    const std::vector<Bone>& bones = mModels[it->second.model].bones;
                    for (std::size_t n = 0; n < bones.size(); ++n)
                        if (bones[n].node && it->second.slots[n] < 0)
                            still += bones[n].name + " ";
                    Log(Debug::Info) << "Kurenai: native body bones kept as modelled: " << still;
                }
                else
                    Log(Debug::Info) << "Kurenai: model " << std::hex << pose->model << std::dec
                                     << ": no native body fits (" << mModels.size() << " loaded); Morrowind's body";
            }
            f = it->second;
        }
        SceneUtil::PositionAttitudeTransform* base = player.isEmpty() ? nullptr : player.getRefData().getBaseNode();
        if (f.model < 0 || !base)
        {
            if (mPlace)
                mPlace->setNodeMask(0);
            hide(false);
            return false;
        }
        attach(player, f.model);
        hide(true);

        // The actor stands at her position facing her heading: bones go into her character space (her position and
        // facing taken out, her tilt kept), then into the actor's (axes, and game units to Morrowind's).
        const float baseScale = base->getScale().x() > 1e-4f ? base->getScale().x() : 1.0f;
        mPlace->setMatrix(kCharToActor * osg::Matrixf::scale(osg::Vec3f(1, 1, 1) * (scale / baseScale)));
        const kurenai::proto::Quat h = kurenai::headingOnly(state->rotation);
        const osg::Matrixf toChar = osg::Matrixf::translate(-state->position.x, -state->position.y, -state->position.z)
            * osg::Matrixf::rotate(osg::Quat(h.x, h.y, h.z, h.w).inverse());

        std::vector<Bone>& bones = mModels[f.model].bones;
        mWorld.assign(bones.size(), osg::Matrixf::identity());
        for (std::size_t n = 0; n < bones.size(); ++n)
        {
            Bone& b = bones[n];
            if (!b.node)
                continue;
            const osg::Matrixf parentWorld = b.parent >= 0 ? mWorld[b.parent] : osg::Matrixf::identity();
            const int s = f.slots[n];
            if (s >= 0 && live(*pose, s))
            {
                const kurenai::proto::Axes& a = pose->axes[s];
                const kurenai::proto::Vec3& t = pose->bones[s];
                const osg::Matrixf game(a.x.x, a.x.y, a.x.z, 0, a.y.x, a.y.y, a.y.z, 0, a.z.x, a.z.y, a.z.z, 0, t.x, t.y,
                    t.z, 1);
                mWorld[n] = game * toChar;
            }
            else
                mWorld[n] = b.rest * parentWorld; // not animated live: as modelled, on its parent
            b.node->setMatrix(mWorld[n] * osg::Matrixf::inverse(parentWorld));
        }

        // Kiton: the renderer fades her to a faint dark shape (REVERSE_ENGINEERING.md, items); her object holds no fade
        // value, so it is applied over the game's own colours here.
        const auto shade = [faint](kurenai::proto::Color c) {
            if (faint)
                c = { c.r * 0.25f, c.g * 0.25f, c.b * 0.25f, c.a * 0.35f };
            return c;
        };
        std::vector<kurenai::proto::Color> colors = pose->colors;
        if (colors.empty())
            colors.resize(mModels[f.model].materials.empty() ? 0 : mModels[f.model].materials.rbegin()->first + 1);
        for (kurenai::proto::Color& c : colors)
            c = shade(c);
        if (colors != mColors)
        {
            mColors = colors;
            applyColors(mModels[f.model].materials, mColors);
            if (!mColors.empty() && mModels[f.model].materials.empty() && !mWarnedNoMaterials)
            {
                mWarnedNoMaterials = true;
                Log(Debug::Warning) << "Kurenai: native body " << mModels[f.model].path
                                    << " has no krn_*_m<N> shapes: kiton fade and damage flash won't show";
            }
        }
        // Her items: each where the game draws it this frame (its own matrix, into her character space), else hidden.
        for (auto& [slot, item] : mItems)
        {
            const kurenai::proto::HeldItem* drawn = nullptr;
            // Her tetsugen: the blade (render object 0) or the fundo (the weight, object 2), whichever is her attachment;
            // the other one is hidden. The game draws the weight in her hand from the start, so the fundo takes its own
            // pose, or the blade's when the game isn't drawing it.
            if ((mAttachment == 1 && slot == 0) || (mAttachment == 0 && slot == 2))
            {
                item.place->setNodeMask(0);
                continue;
            }
            for (const kurenai::proto::HeldItem& it : pose->items)
                if (it.slot == slot)
                    drawn = &it;
            // The game may draw only one of the two ends in her hand; either one's pose (the hand's) serves for the other.
            if (!drawn && (slot == 0 || slot == 2))
                for (const kurenai::proto::HeldItem& it : pose->items)
                    if (it.slot == (slot == 0 ? 2u : 0u))
                        drawn = &it;
            if (!drawn)
            {
                item.place->setNodeMask(0);
                continue;
            }
            if (!item.place->getNumParents())
            {
                mPlace->addChild(item.place);
                Log(Debug::Info) << "Kurenai: native item " << slot << " drawn";
            }
            const kurenai::proto::Axes& a = drawn->axes;
            const kurenai::proto::Vec3& t = drawn->position;
            item.place->setMatrix(osg::Matrixf(a.x.x, a.x.y, a.x.z, 0, a.y.x, a.y.y, a.y.z, 0, a.z.x, a.z.y, a.z.z, 0, t.x,
                                      t.y, t.z, 1)
                * toChar);
            if (item.place->getNodeMask() == 0 && (slot == 0 || slot == 2))
                Log(Debug::Info) << "Kurenai: tetsugen " << (slot == 0 ? "blade" : "fundo") << " shown (attachment " << mAttachment << ")";
            if (item.versions.size() > 1)
            {
                const std::size_t level = static_cast<std::size_t>(slot == 0 ? mBladeLevel : mFundoLevel);
                for (std::size_t v = 0; v < item.versions.size(); ++v)
                    item.versions[v]->setNodeMask(v == std::min(level, item.versions.size() - 1) ? ~0u : 0u);
            }
            item.place->setNodeMask(~0u);
            applyColors(item.materials, { shade(drawn->color) });
        }
        return true;
    }
}
