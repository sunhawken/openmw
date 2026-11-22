#ifndef OPENMW_MWRENDER_EFFECTMANAGER_H
#define OPENMW_MWRENDER_EFFECTMANAGER_H
#include <map>

#include <memory>
#include <vector>

#include <osg/ref_ptr>
#include <osg/Vec3f>
#include <osg/Array>

#include <components/vfs/pathutil.hpp>

namespace osg
{
    class Group;
    class Vec3f;
    class PositionAttitudeTransform;
    class Billboard;
    class StateSet;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWRender
{
    class EffectAnimationTime;
    class ClippedRenderPrewarm;

    // Note: effects attached to another object should be managed by MWRender::Animation::addEffect.
    // This class manages "free" effects, i.e. attached to a dedicated scene node in the world.
    class EffectManager
    {
    public:
        EffectManager(osg::ref_ptr<osg::Group> parent, Resource::ResourceSystem* resourceSystem);
        EffectManager(const EffectManager&) = delete;
        ~EffectManager();

        /// Add an effect. When it's finished playing, it will be removed automatically.
        void addEffect(VFS::Path::NormalizedView model, std::string_view textureOverride,
            const osg::Vec3f& worldPosition, float scale, bool isMagicVFX = true, bool useAmbientLight = true,
            std::string_view effectId = {}, bool loop = false);

        void removeEffect(std::string_view effectId);
        bool spawnBrutalImpact(std::string_view kind, const osg::Vec3f& position,
            const osg::Vec3f& direction, float intensity, bool power);
        bool spawnBrutalDecapitation(const osg::Vec3f& lowerCenter, const osg::Vec3f& lowerNormal,
            const osg::Vec3f& upperCenter, const osg::Vec3f& upperNormal, const osg::Vec3f& inheritedVelocity);
        void queueCutBlood(std::string_view kind,const osg::Vec3f& position,const osg::Vec3f& direction,float intensity);
        std::map<std::string, double> getBrutalImpactStats() const;


        void update(float dt);
        void updateBrutalPrewarm(const osg::Vec3f& eye, const osg::Vec3f& forward, float nearClip);

        /// Remove all effects
        void clear();

    private:
        void prewarmBrutalImpacts();
        // Immutable render templates only. Each spawned particle owns its colors.
        osg::ref_ptr<osg::Billboard> mBrutalImpactTemplates[6];
        std::vector<osg::ref_ptr<osg::StateSet>> mCutSprayFrames;
        bool mBrutalPrewarmAttempted = false;
        double mBrutalPrewarmMilliseconds = 0;
        bool mBrutalCompileQueued[6] = {};
        bool mBrutalWarmDrawAttempted = false;
        std::unique_ptr<ClippedRenderPrewarm> mBrutalWarmDraw;
        osg::ref_ptr<osg::PositionAttitudeTransform> mBrutalWarmTransform;
        struct BrutalParticle
        {
            osg::ref_ptr<osg::PositionAttitudeTransform> transform;
            osg::ref_ptr<osg::Vec4Array> colors;
            osg::ref_ptr<osg::Billboard> animatedBillboard;
            osg::Vec3f position, velocity;
            float age = 0.f, life = 0.5f, size = 1.f;
            bool spark = false;
            float opacity = 1.f;
            int animationFrame = -1;
        };
        std::vector<BrutalParticle> mBrutalParticles;
        struct CutBloodRequest
        {
            std::string kind;
            osg::Vec3f position,direction;
            float intensity;
        };
        std::vector<CutBloodRequest> mPendingCutBlood;
        uint64_t mBrutalBloodCount = 0, mBrutalSparkCount = 0;
        uint64_t mBrutalDecapitationBursts = 0, mBrutalDecapitationParticles = 0;
        osg::Vec3f mBrutalDecapitationLower, mBrutalDecapitationUpper;
        osg::Vec3f mBrutalDecapitationLowerNormal, mBrutalDecapitationUpperNormal;
        osg::Vec3f mBrutalDecapitationVelocity;
        uint32_t mBrutalRandom = 0x17a98c31;
        struct Effect
        {
            std::string mEffectId;
            float mMaxControllerLength;
            bool mLoop;
            std::shared_ptr<EffectAnimationTime> mAnimTime;
            osg::ref_ptr<osg::PositionAttitudeTransform> mTransform;
        };

        std::vector<Effect> mEffects;

        osg::ref_ptr<osg::Group> mParentNode;
        Resource::ResourceSystem* mResourceSystem;
    };

}

#endif
