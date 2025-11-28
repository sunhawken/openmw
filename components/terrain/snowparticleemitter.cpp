#include "snowparticleemitter.hpp"

#include <osgParticle/ParticleSystemUpdater>
#include <osgParticle/ModularProgram>
#include <osgParticle/AccelOperator>
#include <osgParticle/FluidFrictionOperator>
#include <osgParticle/RandomRateCounter>
#include <osgParticle/RadialShooter>
#include <osg/Texture2D>
#include <osg/BlendFunc>
#include <osg/Billboard>
#include <osgDB/ReadFile>

#include <components/resource/scenemanager.hpp>
#include <components/debug/debuglog.hpp>

namespace Terrain
{

    SnowParticleEmitter::SnowParticleEmitter(osg::Group* parentNode, Resource::SceneManager* sceneManager)
        : mParentNode(parentNode)
        , mSceneManager(sceneManager)
    {
        mParticleGroup = new osg::Group;
        mParentNode->addChild(mParticleGroup);

        // Initialize particle system
        mParticleSystem = new osgParticle::ParticleSystem;
        mParticleSystem->getDefaultParticleTemplate().setLifeTime(1.0f);
        mParticleSystem->getDefaultParticleTemplate().setShape(osgParticle::Particle::QUAD);
        mParticleSystem->getDefaultParticleTemplate().setSizeRange(osgParticle::rangef(0.1f, 0.1f));
        mParticleSystem->getDefaultParticleTemplate().setAlphaRange(osgParticle::rangef(1.0f, 0.0f));
        mParticleSystem->getDefaultParticleTemplate().setColorRange(osgParticle::rangev4(
            osg::Vec4(1, 1, 1, 1), osg::Vec4(1, 1, 1, 0)));

        // Set up texture (using a default snow puff texture if available, or a generated one)
        // For now, we'll try to load a standard particle texture or just use a white quad
        // In a real implementation, we should load "textures/tx_snow_flake.dds" or similar
        
        // Setup state
        osg::StateSet* stateset = mParticleSystem->getOrCreateStateSet();
        stateset->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        stateset->setMode(GL_BLEND, osg::StateAttribute::ON);
        stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
        
        osg::BlendFunc* blendFunc = new osg::BlendFunc;
        blendFunc->setFunction(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        stateset->setAttributeAndModes(blendFunc, osg::StateAttribute::ON);

        // Add updater
        osgParticle::ParticleSystemUpdater* updater = new osgParticle::ParticleSystemUpdater;
        updater->addParticleSystem(mParticleSystem);
        mParticleGroup->addChild(updater);

        // Add particle system drawable
        osg::Geode* geode = new osg::Geode;
        geode->addDrawable(mParticleSystem);
        mParticleGroup->addChild(geode);

        // Setup program (physics)
        osgParticle::ModularProgram* program = new osgParticle::ModularProgram;
        program->setParticleSystem(mParticleSystem);
        
        // Gravity
        osgParticle::AccelOperator* accel = new osgParticle::AccelOperator;
        accel->setAcceleration(osg::Vec3(0, 0, -9.8f)); // Gravity
        program->addOperator(accel);

        // Friction (air resistance)
        osgParticle::FluidFrictionOperator* friction = new osgParticle::FluidFrictionOperator;
        friction->setFluidToAir();
        program->addOperator(friction);

        mParticleProgram = program;
        mParticleGroup->addChild(program);

        // Define configs
        mConfigs["snow"] = { "", osg::Vec4(0.9f, 0.9f, 1.0f, 0.8f), 0.15f, 0.8f, 2.0f, 15 };
        mConfigs["ash"] = { "", osg::Vec4(0.3f, 0.3f, 0.3f, 0.8f), 0.15f, 1.0f, 1.5f, 10 };
        mConfigs["mud"] = { "", osg::Vec4(0.4f, 0.3f, 0.2f, 0.9f), 0.1f, 0.5f, 1.0f, 8 };
    }

    SnowParticleEmitter::~SnowParticleEmitter()
    {
        if (mParentNode && mParticleGroup)
        {
            mParentNode->removeChild(mParticleGroup);
        }
    }

    void SnowParticleEmitter::emit(const osg::Vec3f& position, const std::string& terrainType)
    {
        auto it = mConfigs.find(terrainType);
        if (it == mConfigs.end())
            it = mConfigs.find("snow"); // Default

        const ParticleConfig& config = it->second;

        // Create a temporary emitter for this burst
        osgParticle::ModularEmitter* emitter = new osgParticle::ModularEmitter;
        emitter->setParticleSystem(mParticleSystem);
        
        // Setup counter (burst)
        osgParticle::RandomRateCounter* counter = new osgParticle::RandomRateCounter;
        counter->setRateRange(config.count * 10, config.count * 10); // High rate for short time
        emitter->setCounter(counter);

        // Setup placer (point source)
        osgParticle::PointPlacer* placer = new osgParticle::PointPlacer;
        placer->setCenter(position + osg::Vec3(0, 0, 0.1f)); // Slightly above ground
        emitter->setPlacer(placer);

        // Setup shooter (radial spray)
        osgParticle::RadialShooter* shooter = new osgParticle::RadialShooter;
        shooter->setThetaRange(0.0f, osg::PI_4); // Upwards cone
        shooter->setInitialSpeedRange(config.speed * 0.5f, config.speed * 1.5f);
        emitter->setShooter(shooter);

        // Configure particle template for this burst
        // Note: Changing default template affects all new particles. 
        // Ideally we'd have separate systems for different types if we need vastly different textures,
        // but for color/size we can rely on the template at emission time.
        mParticleSystem->getDefaultParticleTemplate().setColorRange(
            osgParticle::rangev4(config.color, osg::Vec4(config.color.r(), config.color.g(), config.color.b(), 0.0f)));
        mParticleSystem->getDefaultParticleTemplate().setSizeRange(
            osgParticle::rangef(config.size * 0.5f, config.size * 1.5f));
        mParticleSystem->getDefaultParticleTemplate().setLifeTime(config.lifeTime);

        // Add emitter to scene
        mParticleGroup->addChild(emitter);

        // We need to remove this emitter after it's done. 
        // For now, we'll let it emit for one frame then remove it?
        // Or better: manually emit particles.
        
        // Manual emission approach is cleaner than managing temporary emitter nodes
        int count = config.count;
        for(int i=0; i<count; ++i)
        {
            osgParticle::Particle* p = mParticleSystem->createParticle(nullptr);
            if(p)
            {
                p->setPosition(position + osg::Vec3(
                    (rand()%100 - 50)/500.0f, 
                    (rand()%100 - 50)/500.0f, 
                    0.1f));
                
                // Random velocity in cone
                float theta = (rand()%100/100.0f) * osg::PI_4; // 0 to 45 degrees from up
                float phi = (rand()%100/100.0f) * osg::PI * 2.0f;
                
                float speed = config.speed * (0.8f + (rand()%100/200.0f));
                
                float r = speed * sin(theta);
                osg::Vec3 velocity(
                    r * cos(phi),
                    r * sin(phi),
                    speed * cos(theta)
                );
                
                p->setVelocity(velocity);
                p->setLifeTime(config.lifeTime * (0.8f + (rand()%100/200.0f)));
                p->setSizeRange(osgParticle::rangef(config.size, config.size * 2.0f));
                p->setColorRange(osgParticle::rangev4(config.color, osg::Vec4(config.color.r(), config.color.g(), config.color.b(), 0.0f)));
                p->setAlphaRange(osgParticle::rangef(config.color.a(), 0.0f));
            }
        }
        
        // Cleanup: We didn't actually add the emitter node, so nothing to remove.
        // We just used the particle system directly.
        delete emitter; 
    }

    void SnowParticleEmitter::update(float dt)
    {
        // Update logic if needed
    }

}
