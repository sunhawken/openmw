#include "snowdeformation.hpp"
#include "snowdetection.hpp"
#include "storage.hpp"

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>

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
    {
        Log(Debug::Info) << "[SNOW] Snow deformation system initialized (vertex shader array approach)";
        Log(Debug::Info) << "[SNOW] Settings: maxFootprints=" << Settings::terrain().mSnowMaxFootprints.get()
                        << ", radius=" << mFootprintRadius
                        << ", depth=" << mDeformationDepth
                        << ", decay=" << mDecayTime << "s";
        Log(Debug::Info) << "[SNOW] System " << (mEnabled ? "enabled" : "disabled") << " by config";

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

        Log(Debug::Info) << "[SNOW] Shader uniforms created (snow, ash, mud)";
    }

    SnowDeformationManager::~SnowDeformationManager()
    {
        Log(Debug::Info) << "[SNOW] Snow deformation system destroyed";
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
            Log(Debug::Info) << "[SNOW] Deformation system " << (mActive ? "activated" : "deactivated");
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
            stampFootprint(playerPos);
            mLastFootprintPos = playerPos;
            mTimeSinceLastFootprint = 0.0f;
        }

        // Update current time uniform
        mCurrentTimeUniform->set(mCurrentTime);
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
            Log(Debug::Info) << "[SNOW] Snow deformation " << (enabled ? "enabled" : "disabled");
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

        static int stampCount = 0;
        stampCount++;
        if (stampCount <= 5 || stampCount % 10 == 0)
        {
            Log(Debug::Info) << "[SNOW] Footprint #" << stampCount << " at ("
                            << (int)position.x() << ", " << (int)position.y() << ")"
                            << " | Total: " << mFootprints.size() << "/" << maxFootprints;
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

                Log(Debug::Info) << "[SNOW] Terrain changed to '" << terrainType
                                << "' - radius=" << params.radius
                                << ", depth=" << params.depth
                                << ", interval=" << params.interval;

                updateShaderUniforms();
                return;
            }
        }

        Log(Debug::Info) << "[SNOW] Unknown terrain '" << terrainType << "', using snow defaults";
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
}
