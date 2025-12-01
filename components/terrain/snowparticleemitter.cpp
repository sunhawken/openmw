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
#include <osg/PointSprite>
#include <osg/Point>
#include <osgDB/ReadFile>

#include <components/resource/scenemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/debug/debuglog.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

namespace Terrain
{

    SnowParticleEmitter::SnowParticleEmitter(osg::Group* parentNode, Resource::SceneManager* sceneManager)
        : mParentNode(parentNode)
        , mSceneManager(sceneManager)
    {
        mParticleGroup = new osg::Group;
        mParentNode->addChild(mParticleGroup);

        // Initialize particle system with snow spray appearance
        mParticleSystem = new osgParticle::ParticleSystem;

        // Default particle template - creates a soft, fluffy snow spray look
        osgParticle::Particle& defaultTemplate = mParticleSystem->getDefaultParticleTemplate();
        defaultTemplate.setLifeTime(1.2f);
        defaultTemplate.setShape(osgParticle::Particle::QUAD); // Billboarded quads
        defaultTemplate.setSizeRange(osgParticle::rangef(8.0f, 25.0f)); // Start small, grow as they disperse
        defaultTemplate.setAlphaRange(osgParticle::rangef(0.7f, 0.0f)); // Fade out smoothly
        defaultTemplate.setColorRange(osgParticle::rangev4(
            osg::Vec4(0.95f, 0.95f, 1.0f, 0.8f),  // Slightly blue-tinted white at start
            osg::Vec4(0.9f, 0.9f, 0.95f, 0.0f))); // Fade to transparent
        defaultTemplate.setMass(0.05f); // Light particles affected by air resistance

        // Setup state for soft, volumetric look
        osg::StateSet* stateset = mParticleSystem->getOrCreateStateSet();
        stateset->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        stateset->setMode(GL_BLEND, osg::StateAttribute::ON);
        stateset->setMode(GL_DEPTH_TEST, osg::StateAttribute::ON);
        stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

        // Additive blending for a more volumetric/glowing look
        osg::BlendFunc* blendFunc = new osg::BlendFunc;
        blendFunc->setFunction(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        stateset->setAttributeAndModes(blendFunc, osg::StateAttribute::ON);

        // Try to load a soft particle texture for better visuals
        // Morrowind has various particle textures we could use
        loadParticleTexture(stateset);

        // Add updater
        osgParticle::ParticleSystemUpdater* updater = new osgParticle::ParticleSystemUpdater;
        updater->addParticleSystem(mParticleSystem);
        mParticleGroup->addChild(updater);

        // Add particle system drawable
        osg::Geode* geode = new osg::Geode;
        geode->addDrawable(mParticleSystem);
        mParticleGroup->addChild(geode);

        // Setup program (physics) - tuned for realistic snow spray
        osgParticle::ModularProgram* program = new osgParticle::ModularProgram;
        program->setParticleSystem(mParticleSystem);

        // Reduced gravity - snow particles float more
        osgParticle::AccelOperator* accel = new osgParticle::AccelOperator;
        accel->setAcceleration(osg::Vec3(0, 0, -4.0f)); // Reduced gravity for floaty feel
        program->addOperator(accel);

        // Strong air resistance for fluffy snow behavior
        osgParticle::FluidFrictionOperator* friction = new osgParticle::FluidFrictionOperator;
        friction->setFluidDensity(1.5f); // Slightly denser than air for drag
        friction->setFluidViscosity(0.00002f);
        program->addOperator(friction);

        mParticleProgram = program;
        mParticleGroup->addChild(program);

        // Define terrain-specific particle configurations
        // Snow: Light, fluffy, white/blue tint, lots of particles, floaty
        mConfigs["snow"] = {
            "textures/tx_spark_white.dds", // Will try to load
            osg::Vec4(0.95f, 0.95f, 1.0f, 0.7f), // Slightly blue-white
            15.0f,  // size - visible particles
            1.2f,   // lifetime
            120.0f, // speed - good initial kick
            25      // count - lots of spray
        };

        // Ash: Darker, slower, gray particles, fewer, heavier feel
        mConfigs["ash"] = {
            "textures/tx_spark_white.dds",
            osg::Vec4(0.4f, 0.4f, 0.45f, 0.8f), // Dark gray with slight blue
            12.0f,  // size
            1.5f,   // longer lifetime (ash lingers)
            80.0f,  // slower speed
            15      // fewer particles
        };

        // Mud: Brown, heavy, splatter effect, fast fall
        mConfigs["mud"] = {
            "textures/tx_spark_white.dds",
            osg::Vec4(0.35f, 0.25f, 0.15f, 0.9f), // Brown
            10.0f,  // smaller particles
            0.6f,   // short lifetime (falls fast)
            100.0f, // good speed
            12      // moderate count
        };
    }

    void SnowParticleEmitter::loadParticleTexture(osg::StateSet* stateset)
    {
        // Try to load a soft particle texture from game assets
        // These are common Morrowind particle textures
        const char* textureAttempts[] = {
            "textures/tx_spark_white.dds",
            "textures/tx_weathersnow_01.dds",
            "textures/tx_frost_particle.dds",
            "textures/tx_smoke_white.dds"
        };

        osg::ref_ptr<osg::Image> image;
        for (const char* texPath : textureAttempts)
        {
            try {
                VFS::Path::Normalized normalizedPath(texPath);
                image = mSceneManager->getImageManager()->getImage(normalizedPath);
                if (image.valid())
                {
                    Log(Debug::Info) << "SnowParticleEmitter: Loaded particle texture: " << texPath;
                    break;
                }
            }
            catch (...) {
                // Texture not found, try next
            }
        }

        if (image.valid())
        {
            osg::Texture2D* tex = new osg::Texture2D(image);
            tex->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
            tex->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
            tex->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::CLAMP_TO_EDGE);
            tex->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::CLAMP_TO_EDGE);
            stateset->setTextureAttributeAndModes(0, tex, osg::StateAttribute::ON);
        }
        else
        {
            Log(Debug::Warning) << "SnowParticleEmitter: No particle texture found, using white quads";
        }
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

        // Emit a burst of particles in a cone pattern
        // This creates a "spray" effect when the foot hits the ground
        int count = config.count;

        for (int i = 0; i < count; ++i)
        {
            osgParticle::Particle* p = mParticleSystem->createParticle(nullptr);
            if (p)
            {
                // Random starting position spread around the footprint
                // Creates a ring-like emission pattern
                float spreadRadius = 10.0f + (rand() % 100) / 10.0f; // 10-20 units from center
                float spreadAngle = (rand() % 3600) / 10.0f * osg::PI / 180.0f; // 0-360 degrees

                osg::Vec3 startOffset(
                    spreadRadius * cos(spreadAngle),
                    spreadRadius * sin(spreadAngle),
                    2.0f + (rand() % 50) / 10.0f // Slightly above ground (2-7 units)
                );

                p->setPosition(position + startOffset);

                // Velocity: Spray outward and upward in a cone
                // The cone angle determines how wide the spray spreads
                float coneAngle = osg::PI / 6.0f; // 30 degree cone half-angle
                float theta = (rand() % 100) / 100.0f * coneAngle; // Angle from up-vector
                float phi = spreadAngle + (rand() % 100 - 50) / 50.0f * osg::PI / 4.0f; // Mostly outward

                // Speed with some randomness
                float speed = config.speed * (0.7f + (rand() % 100) / 150.0f);

                // Calculate velocity components
                // Particles mostly go UP and OUT from the center
                float verticalComponent = speed * cos(theta);
                float horizontalComponent = speed * sin(theta);

                osg::Vec3 velocity(
                    horizontalComponent * cos(phi),
                    horizontalComponent * sin(phi),
                    verticalComponent * 0.8f + (rand() % 100) / 100.0f * speed * 0.3f // Strong upward bias
                );

                p->setVelocity(velocity);

                // Lifetime with variance
                p->setLifeTime(config.lifeTime * (0.6f + (rand() % 100) / 125.0f));

                // Size: Start small, grow slightly as particle disperses
                float baseSize = config.size * (0.8f + (rand() % 100) / 250.0f);
                p->setSizeRange(osgParticle::rangef(baseSize * 0.6f, baseSize * 1.5f));

                // Color: Use config color with slight random variation
                float colorVar = (rand() % 100 - 50) / 500.0f; // +/- 0.1 variation
                osg::Vec4 startColor(
                    std::min(1.0f, std::max(0.0f, config.color.r() + colorVar)),
                    std::min(1.0f, std::max(0.0f, config.color.g() + colorVar)),
                    std::min(1.0f, std::max(0.0f, config.color.b() + colorVar)),
                    config.color.a() * (0.8f + (rand() % 100) / 500.0f) // Alpha variation
                );
                osg::Vec4 endColor(startColor.r(), startColor.g(), startColor.b(), 0.0f);

                p->setColorRange(osgParticle::rangev4(startColor, endColor));
                p->setAlphaRange(osgParticle::rangef(startColor.a(), 0.0f));
            }
        }
    }

    void SnowParticleEmitter::update(float dt)
    {
        // Update logic if needed
    }

}
