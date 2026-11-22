#include <components/misc/brutalconstants.hpp>
#include "effectmanager.hpp"
#include "renderprewarm.hpp"

#include <osg/PositionAttitudeTransform>
#include <osg/Billboard>
#include "skyutil.hpp"
#include <osg/Geometry>
#include <osg/BlendFunc>
#include <osg/Depth>
#include <components/sceneutil/depth.hpp>
#include <osg/Material>
#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/debug/debuglog.hpp>
#include <cmath>
#include <chrono>
#include <osgUtil/IncrementalCompileOperation>


#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/sceneutil/controller.hpp>

#include "animation.hpp"
#include "util.hpp"
#include "vismask.hpp"

#include <algorithm>

namespace MWRender
{

    EffectManager::EffectManager(osg::ref_ptr<osg::Group> parent, Resource::ResourceSystem* resourceSystem)
        : mParentNode(std::move(parent))
        , mResourceSystem(resourceSystem)
    {
    }

    EffectManager::~EffectManager()
    {
        clear();
    }

    void EffectManager::prewarmBrutalImpacts()
    {
        if (mBrutalPrewarmAttempted || !Settings::game().mBrutalImpactEffects)
            return;
        mBrutalPrewarmAttempted = true;
        const auto started = std::chrono::steady_clock::now();
        constexpr const char* paths[]{"textures/brutal_dm/blood.png", "textures/brutal_dm/spark.png",
            "textures/brutal_dm/blood_drop.png", "textures/brutal_dm/blood_core.png",
            "textures/brutal_dm/blood_spray_00.png", "textures/brutal_dm/blood_spatter.png"};
        for (int index = 0; index < 6; ++index)
        {
            const bool spark = index == 1;
            const VFS::Path::NormalizedView path(paths[index]);
            if (!mResourceSystem->getVFS()->exists(path))
                continue;
            try
            {
                osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(mResourceSystem->getImageManager()->getImage(path));
                texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
                texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
                mResourceSystem->getSceneManager()->applyFilterSettings(texture);
                osg::ref_ptr<osg::Billboard> billboard = new osg::Billboard;
                billboard->setMode(osg::Billboard::POINT_ROT_EYE);
                billboard->setNormal(osg::Vec3f(0,-1,0));
                const float height=spark?2.f:index==2?.5f:1.f;
                osg::ref_ptr<osg::Geometry> geometry = osg::createTexturedQuadGeometry(
                    osg::Vec3f(-0.5f,0,-height*.5f), osg::Vec3f(1,0,0), osg::Vec3f(0,0,height));
                geometry->setUseDisplayList(false);
                geometry->setUseVertexBufferObjects(true);
                osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
                colors->push_back(osg::Vec4(0,0,0,1));
                geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
                billboard->addDrawable(geometry);
                auto* state = billboard->getOrCreateStateSet();
                state->setMode(GL_LIGHTING, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                state->setAttributeAndModes(createAlphaTrackingUnlitMaterial(), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
                state->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
                state->setTextureAttribute(0, new SceneUtil::TextureType("diffuseMap"));
                state->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, spark ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA));
                state->setAttributeAndModes(new SceneUtil::AutoDepth(osg::Depth::LEQUAL,0,1,false));
                state->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                mResourceSystem->getSceneManager()->setUpNormalsRTForStateSet(state, false);
                mResourceSystem->getSceneManager()->recreateShaders(billboard);
                if (auto* compile = mResourceSystem->getSceneManager()->getIncrementalCompileOperation())
                {
                    compile->add(billboard);
                    mBrutalCompileQueued[index] = true;
                }
                // This node never enters the scene and consumes no particle RNG.
                mBrutalImpactTemplates[index] = std::move(billboard);
                if(index==4)
                {
                    // Dark Messiah's 47-frame VTF animation, decoded by the importer.
                    // Immutable states are selected by age; no texture IO on a cut.
                    std::vector<osg::ref_ptr<osg::StateSet>> frames;
                    for(unsigned frame=0;frame<47;++frame)
                    {
                        const std::string number=(frame<10?"0":"")+std::to_string(frame);
                        const VFS::Path::Normalized framePath("textures/brutal_dm/blood_spray_"+number+".png");
                        if(!mResourceSystem->getVFS()->exists(framePath)){frames.clear();break;}
                        osg::ref_ptr<osg::Texture2D> image=new osg::Texture2D(mResourceSystem->getImageManager()->getImage(framePath));
                        image->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);
                        image->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
                        mResourceSystem->getSceneManager()->applyFilterSettings(image);
                        osg::ref_ptr<osg::StateSet> frameState=new osg::StateSet(*mBrutalImpactTemplates[index]->getStateSet(),osg::CopyOp::SHALLOW_COPY);
                        frameState->setTextureAttributeAndModes(0,image,osg::StateAttribute::ON);
                        frames.push_back(frameState);
                    }
                    mCutSprayFrames=std::move(frames);
                }
            }
            catch (const std::exception& error)
            {
                Log(Debug::Warning) << "Could not prepare combat impact resources: " << error.what();
            }
        }
        mBrutalPrewarmMilliseconds = std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now() - started).count();
    }

    void EffectManager::updateBrutalPrewarm(const osg::Vec3f& eye, const osg::Vec3f& forward, float nearClip)
    {
        prewarmBrutalImpacts();
        if (!mBrutalPrewarmAttempted)
            return;
        if (!mBrutalWarmDrawAttempted)
        {
            mBrutalWarmDrawAttempted = true;
            mBrutalWarmTransform = new osg::PositionAttitudeTransform;
            for (const auto& prepared : mBrutalImpactTemplates)
            {
                if (!prepared)
                    continue;
                osg::ref_ptr<osg::Billboard> billboard = new osg::Billboard;
                billboard->setMode(osg::Billboard::POINT_ROT_EYE);
                billboard->setNormal(osg::Vec3f(0, -1, 0));
                billboard->setStateSet(prepared->getStateSet());
                billboard->addDrawable(new osg::Geometry(
                    *static_cast<osg::Geometry*>(prepared->getDrawable(0)), osg::CopyOp::SHALLOW_COPY));
                mBrutalWarmTransform->addChild(billboard);
            }
            mBrutalWarmDraw = ClippedRenderPrewarm::attach(mBrutalWarmTransform, mParentNode);
        }
        if (mBrutalWarmDraw && mBrutalWarmTransform)
        {
            // Cull against a small bound in front of the real view; the draw
            // callback clips the actual vertices behind the eye without pixels.
            mBrutalWarmTransform->setPosition(eye + forward * std::max(nearClip * 2.f, 4.f));
            if (mBrutalWarmDraw->update())
                mBrutalWarmTransform = nullptr;
        }
    }

    void EffectManager::addEffect(VFS::Path::NormalizedView model, std::string_view textureOverride,
        const osg::Vec3f& worldPosition, float scale, bool isMagicVFX, bool useAmbientLight, std::string_view effectId,
        bool loop)
    {
        osg::ref_ptr<osg::Node> node = mResourceSystem->getSceneManager()->getInstance(model);

        node->setNodeMask(Mask_Effect);

        Effect effect;
        effect.mAnimTime = std::make_shared<EffectAnimationTime>();
        effect.mLoop = loop;
        effect.mEffectId = effectId;

        SceneUtil::FindMaxControllerLengthVisitor findMaxLengthVisitor;
        node->accept(findMaxLengthVisitor);
        effect.mMaxControllerLength = findMaxLengthVisitor.getMaxLength();

        osg::ref_ptr<osg::PositionAttitudeTransform> trans = new osg::PositionAttitudeTransform;
        trans->setPosition(worldPosition);
        trans->setScale(osg::Vec3f(scale, scale, scale));
        trans->addChild(node);

        effect.mTransform = trans;

        SceneUtil::AssignControllerSourcesVisitor assignVisitor(effect.mAnimTime);
        node->accept(assignVisitor);

        if (isMagicVFX)
            overrideFirstRootTexture(VFS::Path::toNormalized(textureOverride), mResourceSystem, *node);
        else
            overrideTexture(VFS::Path::toNormalized(textureOverride), mResourceSystem, *node);

        mParentNode->addChild(trans);

        if (useAmbientLight)
        {
            // Morrowind has a white ambient light attached to the root VFX node of the scenegraph
            node->getOrCreateStateSet()->setAttributeAndModes(
                getVFXLightModelInstance(), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }

        mResourceSystem->getSceneManager()->setUpNormalsRTForStateSet(node->getOrCreateStateSet(), false);

        mEffects.push_back(std::move(effect));
    }

    std::map<std::string, double> EffectManager::getBrutalImpactStats() const
    {
        const auto* vfs = mResourceSystem->getVFS();
        return { {"enabled", Settings::game().mBrutalImpactEffects ? 1.0 : 0.0},
            {"bloodAvailable", vfs->exists(VFS::Path::NormalizedView("textures/brutal_dm/blood.png")) ? 1.0 : 0.0},
            {"sparksAvailable", vfs->exists(VFS::Path::NormalizedView("textures/brutal_dm/spark.png")) ? 1.0 : 0.0},
            {"spawnedBlood", double(mBrutalBloodCount)}, {"spawnedSparks", double(mBrutalSparkCount)},
            {"cutSprayFrames",double(mCutSprayFrames.size())},
            {"decapBursts", double(mBrutalDecapitationBursts)},
            {"decapParticles", double(mBrutalDecapitationParticles)},
            {"decapLowerX", mBrutalDecapitationLower.x()},
            {"decapLowerY", mBrutalDecapitationLower.y()},
            {"decapLowerZ", mBrutalDecapitationLower.z()},
            {"decapUpperX", mBrutalDecapitationUpper.x()},
            {"decapUpperY", mBrutalDecapitationUpper.y()},
            {"decapUpperZ", mBrutalDecapitationUpper.z()},
            {"decapLowerNormalX", mBrutalDecapitationLowerNormal.x()},
            {"decapLowerNormalY", mBrutalDecapitationLowerNormal.y()},
            {"decapLowerNormalZ", mBrutalDecapitationLowerNormal.z()},
            {"decapUpperNormalX", mBrutalDecapitationUpperNormal.x()},
            {"decapUpperNormalY", mBrutalDecapitationUpperNormal.y()},
            {"decapUpperNormalZ", mBrutalDecapitationUpperNormal.z()},
            {"decapVelocityX", mBrutalDecapitationVelocity.x()},
            {"decapVelocityY", mBrutalDecapitationVelocity.y()},
            {"decapVelocityZ", mBrutalDecapitationVelocity.z()},
            {"alive", double(mBrutalParticles.size())}, {"limit", 512.0},
            {"bloodPrepared", mBrutalImpactTemplates[0] ? 1.0 : 0.0},
            {"sparksPrepared", mBrutalImpactTemplates[1] ? 1.0 : 0.0},
            {"preparationMilliseconds", mBrutalPrewarmMilliseconds},
            {"bloodCompileQueued", mBrutalCompileQueued[0] ? 1.0 : 0.0},
            {"sparksCompileQueued", mBrutalCompileQueued[1] ? 1.0 : 0.0},
            {"warmDrawables", mBrutalWarmDraw ? double(mBrutalWarmDraw->drawableCount()) : 0.0},
            {"warmDrawn", mBrutalWarmDraw && mBrutalWarmDraw->drawn() ? 1.0 : 0.0},
            {"warmTimedOut", mBrutalWarmDraw && mBrutalWarmDraw->timedOut() ? 1.0 : 0.0} };
    }

    bool EffectManager::spawnBrutalImpact(std::string_view kind, const osg::Vec3f& position,
        const osg::Vec3f& direction, float intensity, bool power)
    {
        const bool wallSpark = kind == "wall_sparks";
        const bool spark = kind == "sparks" || wallSpark;
        const int variant=spark?1:kind=="blood"?0:kind=="cutdrop"?2:kind=="cutcore"?3:kind=="cutspray"?4:kind=="cutspatter"?5:-1;
        const bool flow=variant>=2;
        if (!Settings::game().mBrutalEnabled || (spark ? !Settings::game().mBrutalSparkEffects : !Settings::game().mBrutalBloodEffects) || variant<0 || (flow&&intensity<=.0001f) || !Settings::game().mBrutalImpactEffects || mBrutalParticles.size() >= 512)
            return false;
        prewarmBrutalImpacts();
        const auto& prepared = mBrutalImpactTemplates[variant];
        if (!prepared) return false;
        const auto random = [this]() {
            mBrutalRandom ^= mBrutalRandom << 13;
            mBrutalRandom ^= mBrutalRandom >> 17;
            mBrutalRandom ^= mBrutalRandom << 5;
            return float(mBrutalRandom & 0x00ffffff) / float(0x01000000);
        };
        osg::Vec3f normal = direction;
        if (normal.normalize() < 0.001f) normal.set(0,0,1);
        const int count = flow?1:std::clamp(int((spark ? 12.f : 7.f) * std::clamp(intensity, 0.1f, 3.f)
            * (power ? 1.5f : 1.f)), 1, 48);
        for (int i = 0; i < count && mBrutalParticles.size() < 512; ++i)
        {
            BrutalParticle particle;
            particle.spark = spark;
            particle.position = position;
            // Blood drops must read at first-person melee distance: larger and longer-lived.
            particle.life = spark ? 0.25f + 0.35f * random() : 0.5f + 0.6f * random();
            particle.size = (spark ? 2.f + random() * 3.f : 8.f + random() * 10.f) * (power ? 1.25f : 1.f);
            if(flow)
            {
                particle.opacity=std::clamp(intensity,0.f,1.f);
                particle.size=(variant==3?3.f+random()*4.f:variant==2?4.f+random()*3.f:7.f+random()*7.f)*.6f;
                particle.life=variant==3?1.5f:variant==4?47.f/25.f:.5f+random()*.5f;
            }
            const osg::Vec3f spread(random()*2.f-1.f, random()*2.f-1.f, random()*2.f-0.4f);
            if (spark)
            {
                osg::Vec3f tangent = normal ^ osg::Vec3f(0,0,1);
                if (tangent.normalize() < 0.001f) tangent.set(1,0,0);
                const osg::Vec3f up = tangent ^ normal;
                // Scatter across the contact surface; keep outward speed low at first-person melee distance.
                particle.velocity = normal * (wallSpark ? 8.f + 12.f * random() : 30.f + 45.f * random())
                    + tangent * ((random()*2.f-1.f)*150.f)
                    + up * (wallSpark ? 140.f + 140.f * random() : 20.f + 100.f * random());
            }
            else
                particle.velocity = normal * (30.f + 75.f*random()) + spread * 45.f;
            if(flow)particle.velocity=normal*(12.f+random()*24.f)+spread*(variant==3?4.f:12.f);
            particle.transform = new osg::PositionAttitudeTransform;
            particle.transform->setNodeMask(Mask_Effect);
            particle.transform->setPosition(position);
            particle.transform->setScale(osg::Vec3f(particle.size, particle.size, particle.size));
            osg::ref_ptr<osg::Billboard> billboard = new osg::Billboard;
            billboard->setMode(osg::Billboard::POINT_ROT_EYE);
            billboard->setNormal(osg::Vec3f(0,-1,0));
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry(
                *static_cast<osg::Geometry*>(prepared->getDrawable(0)), osg::CopyOp::SHALLOW_COPY);
            particle.colors = new osg::Vec4Array;
            particle.colors->push_back(osg::Vec4(0,0,0,particle.opacity)); // White emission preserves original sprite RGB.
            geometry->setColorArray(particle.colors, osg::Array::BIND_OVERALL);
            billboard->addDrawable(geometry);
            billboard->setStateSet(prepared->getStateSet());
            if(variant==4&&!mCutSprayFrames.empty())
            {
                billboard->setStateSet(mCutSprayFrames.front());particle.animatedBillboard=billboard;particle.animationFrame=0;
            }
            particle.transform->addChild(billboard);
            mParentNode->addChild(particle.transform);
            mBrutalParticles.push_back(std::move(particle));
            if (spark) ++mBrutalSparkCount; else ++mBrutalBloodCount;
        }
        return true;
    }

    bool EffectManager::spawnBrutalDecapitation(const osg::Vec3f& lowerCenter, const osg::Vec3f& lowerNormal,
        const osg::Vec3f& upperCenter, const osg::Vec3f& upperNormal, const osg::Vec3f& inheritedVelocity)
    {
        if (!Settings::game().mBrutalEnabled || !Settings::game().mBrutalBloodEffects) return false;
        // Both cap bursts use the ordinary blood material that was drawn during
        // scene prewarm. Do not load a new shader/texture at the lethal frame.
        constexpr std::size_t particlesPerSurface = 21; // intensity 2, powered
        if (!Settings::game().mBrutalImpactEffects || mBrutalParticles.size() + 2 * particlesPerSurface > 512)
            return false;
        prewarmBrutalImpacts();
        if (!mBrutalImpactTemplates[0])
            return false;
        const std::size_t first = mBrutalParticles.size();
        if (!spawnBrutalImpact("blood", lowerCenter, lowerNormal, 2.f, true)
            || !spawnBrutalImpact("blood", upperCenter, upperNormal, 2.f, true))
            return false;
        // Newly exposed tissue is moving with the severed body/head. Preserve
        // that motion in the emitted drops, in addition to outward cap spray.
        for (std::size_t i = first; i < mBrutalParticles.size(); ++i)
        {
            mBrutalParticles[i].velocity += inheritedVelocity;
            if (i >= first + particlesPerSurface)
                mBrutalParticles[i].velocity.z() += Misc::BrutalHeadSeparationSpeed;
        }
        ++mBrutalDecapitationBursts;
        mBrutalDecapitationParticles += mBrutalParticles.size() - first;
        mBrutalDecapitationLower = lowerCenter;
        mBrutalDecapitationUpper = upperCenter;
        mBrutalDecapitationLowerNormal = lowerNormal;
        mBrutalDecapitationUpperNormal = upperNormal;
        mBrutalDecapitationVelocity = inheritedVelocity;
        return true;
    }

    void EffectManager::removeEffect(std::string_view effectId)
    {
        mEffects.erase(std::remove_if(mEffects.begin(), mEffects.end(),
                           [effectId, this](Effect& effect) {
                               if (effectId == effect.mEffectId)
                               {
                                   mParentNode->removeChild(effect.mTransform);
                                   return true;
                               }

                               return false;
                           }),
            mEffects.end());
    }

    void EffectManager::queueCutBlood(std::string_view kind,const osg::Vec3f& position,
        const osg::Vec3f& direction,float intensity)
    {
        // A bone callback runs inside scene UPDATE. Defer graph insertion to
        // the normal effect update rather than invalidate an ancestor's child iterator.
        if(mPendingCutBlood.size()<512)mPendingCutBlood.push_back({std::string(kind),position,direction,intensity});
    }

    void EffectManager::update(float dt)
    {
        // Build and enqueue GPU resources during scene setup, before the first hit.
        prewarmBrutalImpacts();
        for(const auto& request:mPendingCutBlood)
            spawnBrutalImpact(request.kind,request.position,request.direction,request.intensity,false);
        mPendingCutBlood.clear();
        mBrutalParticles.erase(std::remove_if(mBrutalParticles.begin(), mBrutalParticles.end(),
            [this, dt](BrutalParticle& particle) {
                particle.age += dt;
                if (!Settings::game().mBrutalEnabled || (particle.spark ? !Settings::game().mBrutalSparkEffects : !Settings::game().mBrutalBloodEffects) || particle.age >= particle.life)
                {
                    mParentNode->removeChild(particle.transform);
                    return true;
                }
                particle.velocity.z() -= (particle.spark ? 280.f : 180.f) * dt;
                particle.position += particle.velocity * dt;
                particle.transform->setPosition(particle.position);
                const float fade = 1.f - particle.age / particle.life;
                particle.colors->front().a() = fade * fade * particle.opacity;
                if(particle.animatedBillboard&&!mCutSprayFrames.empty())
                {
                    const int frame=std::min(int(mCutSprayFrames.size())-1,int(particle.age*25.f));
                    if(frame!=particle.animationFrame)
                    {particle.animatedBillboard->setStateSet(mCutSprayFrames[frame]);particle.animationFrame=frame;}
                }
                particle.colors->dirty();
                return false;
            }), mBrutalParticles.end());

        mEffects.erase(std::remove_if(mEffects.begin(), mEffects.end(),
                           [dt, this](Effect& effect) {
                               bool remove = false;
                               effect.mAnimTime->addTime(dt);
                               if (effect.mAnimTime->getTime() >= effect.mMaxControllerLength)
                               {
                                   if (effect.mLoop)
                                   {
                                       float remainder = effect.mAnimTime->getTime() - effect.mMaxControllerLength;
                                       effect.mAnimTime->resetTime(remainder);
                                   }
                                   else
                                   {
                                       mParentNode->removeChild(effect.mTransform);
                                       remove = true;
                                   }
                               }

                               return remove;
                           }),
            mEffects.end());
    }

    void EffectManager::clear()
    {
        mPendingCutBlood.clear();
        for (const auto& particle : mBrutalParticles) mParentNode->removeChild(particle.transform);
        mBrutalParticles.clear();

        for (const auto& effect : mEffects)
        {
            mParentNode->removeChild(effect.mTransform);
        }
        mEffects.clear();
    }

}
