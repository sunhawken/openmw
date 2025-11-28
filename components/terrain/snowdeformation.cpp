#include "snowdeformation.hpp"
#include "snowdetection.hpp"
#include "storage.hpp"
#include <algorithm>

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>

#include <osg/Texture2D>
#include <osg/FrameBufferObject>
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Depth>
#include <osg/BlendFunc>
#include <osgDB/ReadFile>

namespace Terrain
{
    SnowDeformationManager::SnowDeformationManager(
        Resource::SceneManager* sceneManager,
        Storage* terrainStorage,
        osg::Group* rootNode)
        : mSceneManager(sceneManager)
        , mTerrainStorage(terrainStorage)
        , mWorldspace(ESM::RefId())
        , mEnabled(Settings::terrain().mSnowDeformationEnabled.get())
        , mActive(false)
        , mFootprintRadius(Settings::terrain().mSnowFootprintRadius.get())
        , mFootprintInterval(2.0f)
        , mDeformationDepth(Settings::terrain().mSnowDeformationDepth.get())
        , mLastFootprintPos(0.0f, 0.0f, 0.0f)
        , mTimeSinceLastFootprint(999.0f)
        , mDecayTime(Settings::terrain().mSnowDecayTime.get())
        , mCurrentTerrainType("snow")
        , mCurrentTime(0.0f)
        , mRTTSize(50.0f) // 50 meters coverage
        , mRTTCenter(0.0f, 0.0f, 0.0f)
    {
        Log(Debug::Info) << "Multi-terrain deformation system initialized (snow/ash/mud)";

        // Initialize RTT
        initRTT();

        // Load snow detection patterns
        SnowDetection::loadSnowPatterns();

        // Initialize terrain-based parameters from settings
        mTerrainParams = {
            {
                Settings::terrain().mSnowFootprintRadius.get(),
                Settings::terrain().mSnowDeformationDepth.get(),
                2.0f,  // interval
                "snow"
            },
            {
                Settings::terrain().mAshFootprintRadius.get(),
                Settings::terrain().mAshDeformationDepth.get(),
                3.0f,  // interval
                "ash"
            },
            {
                Settings::terrain().mMudFootprintRadius.get(),
                Settings::terrain().mMudDeformationDepth.get(),
                5.0f,  // interval
                "mud"
            }
        };

        // Create shader uniforms (use configured max footprints)
        int maxFootprints = Settings::terrain().mSnowMaxFootprints.get();
        mFootprintPositionsUniform = new osg::Uniform(osg::Uniform::FLOAT_VEC3, "snowFootprintPositions", maxFootprints);
        mFootprintCountUniform = new osg::Uniform("snowFootprintCount", 0);
        mFootprintRadiusUniform = new osg::Uniform("snowFootprintRadius", mFootprintRadius);
        mDeformationDepthUniform = new osg::Uniform("snowDeformationDepth", mDeformationDepth);
        mAshDeformationDepthUniform = new osg::Uniform("ashDeformationDepth", Settings::terrain().mAshDeformationDepth.get());
        mMudDeformationDepthUniform = new osg::Uniform("mudDeformationDepth", Settings::terrain().mMudDeformationDepth.get());
        mCurrentTimeUniform = new osg::Uniform("snowCurrentTime", 0.0f);
        mDecayTimeUniform = new osg::Uniform("snowDecayTime", mDecayTime);

        // Initialize particle emitter
        mParticleEmitter = std::make_unique<SnowParticleEmitter>(rootNode, sceneManager);
    }

    SnowDeformationManager::~SnowDeformationManager()
    {
    }

    void SnowDeformationManager::update(float dt, const osg::Vec3f& playerPos)
    {
        if (!mEnabled)
            return;

        mCurrentTime += dt;

        // Check if we should be active
        bool shouldActivate = shouldBeActive(playerPos);

        if (shouldActivate != mActive)
        {
            mActive = shouldActivate;
        }

        if (!mActive)
            return;

        // Update terrain-specific parameters
        updateTerrainParameters(playerPos);

        // Check if player has moved enough for a new footprint
        mTimeSinceLastFootprint += dt;

        float distanceMoved = (playerPos - mLastFootprintPos).length();

        if (distanceMoved > mFootprintInterval || mTimeSinceLastFootprint > 0.5f)
        {
            Log(Debug::Verbose) << "SnowDeformationManager::update - Stamping footprint at " << playerPos;
            stampFootprint(playerPos);
            mLastFootprintPos = playerPos;
            mTimeSinceLastFootprint = 0.0f;
        }

        // Update current time uniform
        mCurrentTimeUniform->set(mCurrentTime);

        // Update RTT
        updateRTT(playerPos);
    }

    bool SnowDeformationManager::shouldBeActive(const osg::Vec3f& worldPos)
    {
        if (!mEnabled)
            return false;

        // Detect terrain type at current position
        SnowDetection::TerrainType terrainType = SnowDetection::detectTerrainType(
            worldPos, mTerrainStorage, mWorldspace);

        // Check if the detected terrain type is enabled in settings
        switch (terrainType)
        {
            case SnowDetection::TerrainType::Snow:
                return Settings::terrain().mSnowDeformationEnabled.get();
            case SnowDetection::TerrainType::Ash:
                return Settings::terrain().mAshDeformationEnabled.get();
            case SnowDetection::TerrainType::Mud:
                return Settings::terrain().mMudDeformationEnabled.get();
            default:
                return false;
        }
    }

    void SnowDeformationManager::setEnabled(bool enabled)
    {
        if (mEnabled != enabled)
        {
            mEnabled = enabled;

            if (!enabled)
            {
                mActive = false;
                mFootprints.clear();
                updateShaderUniforms();
            }
        }
    }

    void SnowDeformationManager::setWorldspace(ESM::RefId worldspace)
    {
        mWorldspace = worldspace;
    }

    void SnowDeformationManager::stampFootprint(const osg::Vec3f& position)
    {
        // Add new footprint (X, Y, timestamp)
        mFootprints.push_back(osg::Vec3f(position.x(), position.y(), mCurrentTime));

        // Remove oldest if exceeded configured limit
        size_t maxFootprints = Settings::terrain().mSnowMaxFootprints.get();
        if (mFootprints.size() > maxFootprints)
        {
            mFootprints.pop_front();
        }

        // Update shader uniforms
        updateShaderUniforms();

        // Emit particles
        if (mParticleEmitter)
        {
            mParticleEmitter->emit(position, mCurrentTerrainType);
        }
    }

    void SnowDeformationManager::updateShaderUniforms()
    {
        // Update footprint count
        mFootprintCountUniform->set(static_cast<int>(mFootprints.size()));

        // Update footprint positions array
        for (size_t i = 0; i < mFootprints.size(); ++i)
        {
            mFootprintPositionsUniform->setElement(i, mFootprints[i]);
        }

        // Update other parameters
        mFootprintRadiusUniform->set(mFootprintRadius);
        mDeformationDepthUniform->set(mDeformationDepth);
        mDecayTimeUniform->set(mDecayTime);
    }

    void SnowDeformationManager::updateTerrainParameters(const osg::Vec3f& playerPos)
    {
        std::string terrainType = detectTerrainTexture(playerPos);

        if (terrainType == mCurrentTerrainType)
            return;

        mCurrentTerrainType = terrainType;

        for (const auto& params : mTerrainParams)
        {
            if (terrainType.find(params.pattern) != std::string::npos)
            {
                mFootprintRadius = params.radius;
                mDeformationDepth = params.depth;
                mFootprintInterval = params.interval;

                updateShaderUniforms();
                return;
            }
        }
    }

    std::string SnowDeformationManager::detectTerrainTexture(const osg::Vec3f& worldPos)
    {
        // Use the SnowDetection system to detect terrain type
        SnowDetection::TerrainType terrainType = SnowDetection::detectTerrainType(
            worldPos, mTerrainStorage, mWorldspace);

        switch (terrainType)
        {
            case SnowDetection::TerrainType::Snow:
                return "snow";
            case SnowDetection::TerrainType::Ash:
                return "ash";
            case SnowDetection::TerrainType::Mud:
                return "mud";
            default:
                return "snow";  // Default fallback
        }
    }


    void SnowDeformationManager::initRTT()
    {
        // 1. Create Texture
        mDeformationMap = new osg::Texture2D;
        mDeformationMap->setTextureSize(2048, 2048);
        mDeformationMap->setInternalFormat(GL_RGBA16F_ARB); // Float texture for precision
        mDeformationMap->setSourceFormat(GL_RGBA);
        mDeformationMap->setSourceType(GL_FLOAT);
        mDeformationMap->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
        mDeformationMap->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
        mDeformationMap->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::CLAMP_TO_BORDER);
        mDeformationMap->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::CLAMP_TO_BORDER);
        mDeformationMap->setBorderColor(osg::Vec4(0, 0, 0, 0)); // Default to no deformation

        // 2. Create Camera
        mRTTCamera = new osg::Camera;
        mRTTCamera->setClearColor(osg::Vec4(0, 0, 0, 0)); // Clear to 0 deformation
        mRTTCamera->setRenderOrder(osg::Camera::PRE_RENDER);
        mRTTCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mRTTCamera->setViewport(0, 0, 2048, 2048);
        mRTTCamera->attach(osg::Camera::COLOR_BUFFER, mDeformationMap);
        
        // 3. Create Scene
        mRTTScene = new osg::Group;
        mRTTCamera->addChild(mRTTScene);

        // Add camera to scene graph
        // Since we don't have easy access to root node in constructor without changing signature everywhere,
        // we rely on mRootNode which we added to the class.
        if (mRootNode)
        {
            mRootNode->addChild(mRTTCamera);
        }
        else
        {
            Log(Debug::Error) << "SnowDeformationManager: Root node is null, RTT will not update!";
        }
        
        // 4. Create Uniforms
        mDeformationMapUniform = new osg::Uniform(osg::Uniform::SAMPLER_2D, "snowDeformationMap");
        mDeformationMapUniform->set(mDeformationMap);
        
        mRTTWorldOriginUniform = new osg::Uniform("snowRTTWorldOrigin", osg::Vec3f(0,0,0));
        mRTTScaleUniform = new osg::Uniform("snowRTTScale", mRTTSize);
    }

    void SnowDeformationManager::updateRTT(const osg::Vec3f& playerPos)
    {
        if (!mRTTCamera) return;

        // Center RTT on player
        mRTTCenter = playerPos;
        mRTTWorldOriginUniform->set(mRTTCenter);

        // Update Camera Projection to cover area around player
        double halfSize = mRTTSize / 2.0;
        mRTTCamera->setProjectionMatrixAsOrtho2D(
            playerPos.x() - halfSize, playerPos.x() + halfSize,
            playerPos.y() - halfSize, playerPos.y() + halfSize
        );
        // Look down from high up
        mRTTCamera->setViewMatrixAsLookAt(
            osg::Vec3d(0, 0, 10000), // Eye
            osg::Vec3d(0, 0, 0),     // Center
            osg::Vec3d(0, 1, 0)      // Up
        );
        // mRTTCamera->setViewMatrix(osg::Matrix::identity()); // REMOVED: This was overwriting the LookAt matrix!

        Log(Debug::Verbose) << "SnowDeformationManager::updateRTT - Center: " << mRTTCenter << " Footprints: " << mFootprints.size();

        // Rebuild RTT Scene
        if (mRTTScene->getNumChildren() > 0)
            mRTTScene->removeChildren(0, mRTTScene->getNumChildren());

        if (mFootprints.empty()) return;

        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        
        float radius = mFootprintRadius; 
        
        for (const auto& fp : mFootprints)
        {
            float x = fp.x();
            float y = fp.y();
            float timestamp = fp.z();
            
            // Calculate alpha based on decay
            float age = mCurrentTime - timestamp;
            float alpha = 1.0f - std::max(0.0f, std::min(1.0f, age / mDecayTime));
            
            if (alpha <= 0.0f) continue;

            // Add quad vertices (World Space)
            verts->push_back(osg::Vec3(x - radius, y - radius, 0));
            verts->push_back(osg::Vec3(x + radius, y - radius, 0));
            verts->push_back(osg::Vec3(x + radius, y + radius, 0));
            verts->push_back(osg::Vec3(x - radius, y + radius, 0));
            
            // Color (Red channel = depth/mask)
            colors->push_back(osg::Vec4(1, 0, 0, alpha));
            colors->push_back(osg::Vec4(1, 0, 0, alpha));
            colors->push_back(osg::Vec4(1, 0, 0, alpha));
            colors->push_back(osg::Vec4(1, 0, 0, alpha));
        }

        if (verts->empty()) return;

        geom->setVertexArray(verts);
        geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
        geom->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, verts->size()));
        
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(geom);
        mRTTScene->addChild(geode);
    }

    void SnowDeformationManager::addFootprintToRTT(const osg::Vec3f& position, float rotation)
    {
        // Not used in "rebuild every frame" approach
    }
}
