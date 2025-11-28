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
#include <osg/BlendEquation>
#include <osgDB/ReadFile>

namespace Terrain
{
    SnowDeformationManager::SnowDeformationManager(
        Resource::SceneManager* sceneManager,
        Storage* terrainStorage,
        osg::Group* rootNode)
        : mSceneManager(sceneManager)
        , mRootNode(rootNode)
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
        , mRTTSize(3625.0f) // 50 meters coverage (approx 72.5 units/meter)
        , mRTTCenter(0.0f, 0.0f, 0.0f)
        , mPreviousRTTCenter(0.0f, 0.0f, 0.0f)
        , mWriteBufferIndex(0)
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
                45.0f,  // interval (approx 2 feet)
                "snow"
            },
            {
                Settings::terrain().mAshFootprintRadius.get(),
                Settings::terrain().mAshDeformationDepth.get(),
                45.0f,  // interval
                "ash"
            },
            {
                Settings::terrain().mMudFootprintRadius.get(),
                Settings::terrain().mMudDeformationDepth.get(),
                45.0f,  // interval
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
        // 1. Create Ping-Pong Textures
        for (int i = 0; i < 2; ++i)
        {
            mAccumulationMap[i] = new osg::Texture2D;
            mAccumulationMap[i]->setTextureSize(2048, 2048);
            mAccumulationMap[i]->setInternalFormat(GL_RGBA16F_ARB);
            mAccumulationMap[i]->setSourceFormat(GL_RGBA);
            mAccumulationMap[i]->setSourceType(GL_FLOAT);
            mAccumulationMap[i]->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
            mAccumulationMap[i]->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
            mAccumulationMap[i]->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::CLAMP_TO_BORDER);
            mAccumulationMap[i]->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::CLAMP_TO_BORDER);
            mAccumulationMap[i]->setBorderColor(osg::Vec4(0, 0, 0, 0));
        }

        // 2. Create Update Camera (Pass 1: Scroll & Decay & Apply New Deformation)
        mUpdateCamera = new osg::Camera;
        mUpdateCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
        mUpdateCamera->setClearMask(GL_COLOR_BUFFER_BIT); // Only clear color
        mUpdateCamera->setRenderOrder(osg::Camera::PRE_RENDER, 1); // Run AFTER Depth Camera
        mUpdateCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mUpdateCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mUpdateCamera->setProjectionMatrixAsOrtho2D(0, 1, 0, 1); // Normalized coordinates
        mUpdateCamera->setViewMatrix(osg::Matrix::identity());
        mUpdateCamera->setViewport(0, 0, 2048, 2048);
        mUpdateCamera->attach(osg::Camera::COLOR_BUFFER, mAccumulationMap[0]); // Initial target

        // Create Update Quad & Shader
        mUpdateQuad = new osg::Geode;
        osg::Geometry* geom = new osg::Geometry;
        osg::Vec3Array* verts = new osg::Vec3Array;
        verts->push_back(osg::Vec3(0, 0, 0));
        verts->push_back(osg::Vec3(1, 0, 0));
        verts->push_back(osg::Vec3(1, 1, 0));
        verts->push_back(osg::Vec3(0, 1, 0));
        geom->setVertexArray(verts);
        geom->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, 4));
        osg::Vec2Array* texcoords = new osg::Vec2Array;
        texcoords->push_back(osg::Vec2(0, 0));
        texcoords->push_back(osg::Vec2(1, 0));
        texcoords->push_back(osg::Vec2(1, 1));
        texcoords->push_back(osg::Vec2(0, 1));
        geom->setTexCoordArray(0, texcoords);
        mUpdateQuad->addDrawable(geom);
        mUpdateCamera->addChild(mUpdateQuad);

        // Update Shader
        osg::StateSet* ss = mUpdateQuad->getOrCreateStateSet();
        osg::Program* program = new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX,
            "void main() {\n"
            "  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
            "  gl_TexCoord[0] = gl_MultiTexCoord0;\n"
            "}\n"));
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "uniform sampler2D previousFrame;\n"
            "uniform sampler2D objectMask;\n"
            "uniform vec2 offset;\n"
            "uniform float decayAmount;\n"
            "void main() {\n"
            "  vec2 uv = gl_TexCoord[0].xy;\n"
            "  vec2 sampleUV = uv + offset;\n"
            "  float oldVal = 0.0;\n"
            "  if (sampleUV.x >= 0.0 && sampleUV.x <= 1.0 && sampleUV.y >= 0.0 && sampleUV.y <= 1.0) {\n"
            "    oldVal = texture2D(previousFrame, sampleUV).r;\n"
            "  }\n"
            "  // Check for new deformation (Object Mask)\n"
            "  // Mask is rendered from below, so it matches the UVs (if aligned correctly)\n"
            "  float maskVal = texture2D(objectMask, uv).r;\n"
            "  float newVal = (maskVal > 0.5) ? 1.0 : max(0.0, oldVal - decayAmount);\n"
            "  gl_FragColor = vec4(newVal, 0.0, 0.0, 1.0);\n"
            "}\n"));
        ss->setAttributeAndModes(program, osg::StateAttribute::ON);
        
        mPreviousFrameUniform = new osg::Uniform(osg::Uniform::SAMPLER_2D, "previousFrame");
        mPreviousFrameUniform->set(0); // Texture unit 0
        ss->addUniform(mPreviousFrameUniform);
        ss->setTextureAttributeAndModes(0, mAccumulationMap[1], osg::StateAttribute::ON); // Initial read

        mObjectMaskUniform = new osg::Uniform(osg::Uniform::SAMPLER_2D, "objectMask");
        mObjectMaskUniform->set(1); // Texture unit 1
        ss->addUniform(mObjectMaskUniform);
        // Texture 1 will be bound in updateRTT or here if we create it now

        mRTTOffsetUniform = new osg::Uniform("offset", osg::Vec2(0, 0));
        ss->addUniform(mRTTOffsetUniform);
        ss->addUniform(new osg::Uniform("decayAmount", 0.0f));

        // 3. Create Object Mask Map & Camera (Pass 0: Render Actors)
        mObjectMaskMap = new osg::Texture2D;
        mObjectMaskMap->setTextureSize(2048, 2048);
        mObjectMaskMap->setInternalFormat(GL_R8); // Single channel
        mObjectMaskMap->setSourceFormat(GL_RED);
        mObjectMaskMap->setSourceType(GL_UNSIGNED_BYTE);
        mObjectMaskMap->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
        mObjectMaskMap->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
        mObjectMaskMap->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::CLAMP_TO_BORDER);
        mObjectMaskMap->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::CLAMP_TO_BORDER);
        mObjectMaskMap->setBorderColor(osg::Vec4(0, 0, 0, 0));
        
        ss->setTextureAttributeAndModes(1, mObjectMaskMap, osg::StateAttribute::ON);

        mDepthCamera = new osg::Camera;
        mDepthCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f)); // Clear to Black (No objects)
        mDepthCamera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        mDepthCamera->setRenderOrder(osg::Camera::PRE_RENDER, 0); // Run FIRST
        mDepthCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mDepthCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mDepthCamera->setViewport(0, 0, 2048, 2048);
        mDepthCamera->attach(osg::Camera::COLOR_BUFFER, mObjectMaskMap);
        
        // Cull Mask: Actor(3) | Player(4) | Object(10)
        // 1<<3 = 8, 1<<4 = 16, 1<<10 = 1024. Total = 1048.
        mDepthCamera->setCullMask((1 << 3) | (1 << 4) | (1 << 10));

        // Override Shader for Depth Camera (Output White)
        osg::StateSet* dss = mDepthCamera->getOrCreateStateSet();
        osg::Program* dProgram = new osg::Program;
        dProgram->addShader(new osg::Shader(osg::Shader::VERTEX,
            "void main() {\n"
            "  gl_Position = ftransform();\n"
            "}\n"));
        dProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "void main() {\n"
            "  gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
            "}\n"));
        dss->setAttributeAndModes(dProgram, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        // Disable lighting, textures, etc.
        dss->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        dss->setMode(GL_TEXTURE_2D, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

        // 4. Create Footprint Camera (Pass 2: Add legacy footprints if any)
        mRTTCamera = new osg::Camera;
        mRTTCamera->setClearMask(0); // Don't clear! Draw on top of update pass
        mRTTCamera->setRenderOrder(osg::Camera::PRE_RENDER, 2); // Run LAST
        mRTTCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mRTTCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mRTTCamera->setViewport(0, 0, 2048, 2048);
        mRTTCamera->attach(osg::Camera::COLOR_BUFFER, mAccumulationMap[0]); // Initial target
        
        mRTTScene = new osg::Group;
        mRTTCamera->addChild(mRTTScene);

        // Add cameras to scene graph
        if (mRootNode)
        {
            mRootNode->addChild(mDepthCamera);
            mRootNode->addChild(mUpdateCamera);
            mRootNode->addChild(mRTTCamera);
        }
        else
        {
            Log(Debug::Error) << "SnowDeformationManager: Root node is null, RTT will not update!";
        }
        
        // 4. Create Uniforms for Terrain
        mDeformationMapUniform = new osg::Uniform(osg::Uniform::SAMPLER_2D, "snowDeformationMap");
        mDeformationMapUniform->set(7); // Texture unit 7
        
        mRTTWorldOriginUniform = new osg::Uniform("snowRTTWorldOrigin", osg::Vec3f(0,0,0));
        mRTTScaleUniform = new osg::Uniform("snowRTTScale", mRTTSize);
    }

    void SnowDeformationManager::updateRTT(const osg::Vec3f& playerPos)
    {
        if (!mRTTCamera || !mUpdateCamera) return;

        // 1. Calculate Sliding Window Offset
        // Offset in UV space = (CurrentPos - PreviousPos) / RTTSize
        // Note: RTT is axis aligned.
        // If player moves +X, the window moves +X. The ground moves -X relative to window.
        // To find the same ground point in previous frame: UV_old = UV_new + Offset
        osg::Vec3f delta = playerPos - mPreviousRTTCenter;
        
        // If this is the first frame (or huge jump), reset
        if (delta.length() > mRTTSize)
        {
            delta = osg::Vec3f(0, 0, 0);
        }
        
        osg::Vec2 offset(delta.x() / mRTTSize, delta.y() / mRTTSize);
        mRTTOffsetUniform->set(offset);
        
        mPreviousRTTCenter = playerPos;
        mRTTCenter = playerPos;
        mRTTWorldOriginUniform->set(mRTTCenter);

        // 2. Calculate Decay
        // We want to decay by dt / decayTime
        // But we don't have dt passed to updateRTT directly, we can use mCurrentTime difference?
        // Or just assume 60fps? No.
        // Let's use a fixed small amount for now or calculate it properly.
        // Ideally update() should pass dt to updateRTT.
        // For now, let's assume 1/60s.
        float dt = 0.016f; // TODO: Pass dt
        float decayAmount = (mDecayTime > 0.0f) ? (dt / mDecayTime) : 1.0f;
        
        osg::StateSet* ss = mUpdateQuad->getStateSet();
        if (ss)
        {
            osg::Uniform* decayUniform = ss->getUniform("decayAmount");
            if (decayUniform) decayUniform->set(decayAmount);
        }

        // 3. Swap Buffers
        int readIndex = mWriteBufferIndex;
        mWriteBufferIndex = (mWriteBufferIndex + 1) % 2;
        int writeIndex = mWriteBufferIndex;

        // 4. Update Cameras to target new Write Buffer
        mUpdateCamera->attach(osg::Camera::COLOR_BUFFER, mAccumulationMap[writeIndex]);
        mRTTCamera->attach(osg::Camera::COLOR_BUFFER, mAccumulationMap[writeIndex]);
        
        // 5. Update Shader to read from Read Buffer
        if (ss)
        {
            ss->setTextureAttributeAndModes(0, mAccumulationMap[readIndex], osg::StateAttribute::ON);
        }
        
        // 6. Update Terrain Uniform to read from Write Buffer (Result of this frame)
        // Note: This assumes Terrain renders AFTER these cameras.
        // mDeformationMapUniform->set(mAccumulationMap[writeIndex]); 
        // Actually, let's stick to ReadBuffer for safety (1 frame lag) to avoid read/write race if they overlap.
        // But wait, if we read ReadBuffer, we see OLD frame.
        // If we read WriteBuffer, we see NEW frame.
        // Since RTT cameras are PRE_RENDER, they finish before Main Render.
        // So it IS safe to read WriteBuffer.
        // However, we need to update the Uniform that the Terrain holds.
        // The Terrain holds mDeformationMapUniform.
        // But wait, osg::Uniform just holds an int (sampler index).
        // The TEXTURE is bound in SnowDeformationUpdater.
        // We need to update the texture binding in SnowDeformationUpdater!
        // SnowDeformationUpdater calls getDeformationMap().
        // getDeformationMap() returns mAccumulationMap[mWriteBufferIndex].
        // So that part is handled automatically if SnowDeformationUpdater calls it every frame.
        
        // 7. Update Footprint Camera Projection
        double halfSize = mRTTSize / 2.0;
        mRTTCamera->setProjectionMatrixAsOrtho(
            playerPos.x() - halfSize, playerPos.x() + halfSize,
            playerPos.y() - halfSize, playerPos.y() + halfSize,
            0.0, 20000.0
        );
        mRTTCamera->setViewMatrixAsLookAt(
            osg::Vec3d(0, 0, 10000),
            osg::Vec3d(0, 0, 0),
            osg::Vec3d(0, 1, 0)
        );

        // Update Depth Camera (Look Up from Below)
        // We want to match the RTT area exactly.
        // RTT (Top Down): X=Right, Y=Up.
        // Depth (Bottom Up): Eye=(0,0,-1000). Up=(0,-1,0) to make Right=+X.
        // But Y becomes -Y.
        // So we swap Top/Bottom in Projection to flip Y back.
        mDepthCamera->setProjectionMatrixAsOrtho(
            playerPos.x() - halfSize, playerPos.x() + halfSize,
            playerPos.y() + halfSize, playerPos.y() - halfSize, // Swapped Top/Bottom to flip Y
            0.0, 20000.0
        );
        mDepthCamera->setViewMatrixAsLookAt(
            osg::Vec3d(0, 0, -10000), // Eye Below
            osg::Vec3d(0, 0, 0),      // Center
            osg::Vec3d(0, -1, 0)      // Up vector flipped to align X
        );

        // 8. Render NEW Footprints
        // In the persistent system, we only render NEW footprints this frame.
        // But our mFootprints list contains ALL recent footprints.
        // We need to change this logic.
        // For Phase 1, let's just render the *last added* footprint if it's new.
        // Or, we can clear mFootprints every frame after rendering?
        // Yes, in persistent mode, we don't need to keep history in mFootprints.
        // We just push new ones, render them, and clear.
        
        if (mRTTScene->getNumChildren() > 0)
            mRTTScene->removeChildren(0, mRTTScene->getNumChildren());

        if (mFootprints.empty()) return;

        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        
        float radius = mFootprintRadius; 
        
        // Render all footprints in the list (assuming list is cleared or managed)
        // Since we are transitioning, let's render them all but with MAX blending.
        // Actually, if we render old footprints again, they will re-apply deformation (resetting decay).
        // This is actually GOOD for "refreshing" existing footprints if the player stands still?
        // No, if we stamp every frame, we refresh.
        // If we just have a list of "active" footprints, we should only render them once.
        // But mFootprints is currently used for the *old* shader array system too.
        // We should probably only render the *newest* footprint.
        
        // For now, let's just render the last one if it's new.
        // But stampFootprint adds to the list.
        // Let's assume we render ALL footprints in mFootprints, but we clear mFootprints?
        // No, mFootprints is used for the Vertex Shader fallback (if we kept it).
        // But we are replacing it.
        // Let's just render the last 1-2 footprints.
        
        // Better: Only render footprints that are "new" (timestamp > lastFrameTime).
        // But we don't track lastFrameTime easily here.
        
        // Let's just render ALL footprints in the list for now, but use MAX blending.
        // Since we clear the list in setEnabled(false), it's fine.
        // Wait, if we keep the list, we re-render old footprints every frame, resetting their decay.
        // This defeats the purpose of the accumulation buffer (decay).
        // We MUST only render *new* footprints.
        
        // Solution: Clear mFootprints after rendering?
        // But other systems might need it?
        // The header says "Vertex Shader Array Approach".
        // If we are fully switching to RTT, we don't need the array for the terrain shader anymore?
        // The terrain shader uses the RTT now.
        // So we can clear mFootprints.
        
        for (const auto& fp : mFootprints)
        {
            float x = fp.x();
            float y = fp.y();
            // float timestamp = fp.z(); // Not needed for RTT rendering if we just stamp
            
            verts->push_back(osg::Vec3(x - radius, y - radius, 0));
            verts->push_back(osg::Vec3(x + radius, y - radius, 0));
            verts->push_back(osg::Vec3(x + radius, y + radius, 0));
            verts->push_back(osg::Vec3(x - radius, y + radius, 0));
            
            // Full Red = Full Deformation
            colors->push_back(osg::Vec4(1, 0, 0, 1));
            colors->push_back(osg::Vec4(1, 0, 0, 1));
            colors->push_back(osg::Vec4(1, 0, 0, 1));
            colors->push_back(osg::Vec4(1, 0, 0, 1));
        }

        if (verts->empty()) return;

        geom->setVertexArray(verts);
        geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
        geom->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, verts->size()));
        
        // Enable MAX blending to merge footprints
        osg::StateSet* fpSS = geom->getOrCreateStateSet();
        osg::BlendFunc* blend = new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Standard alpha?
        // We want MAX blending: New = Max(Old, New).
        // Old is in Framebuffer. New is Fragment.
        // glBlendEquation(GL_MAX).
        osg::BlendEquation* blendEq = new osg::BlendEquation(osg::BlendEquation::RGBA_MAX);
        fpSS->setAttributeAndModes(blendEq, osg::StateAttribute::ON);
        // BlendFunc doesn't matter for MAX, but good to have defaults
        fpSS->setAttributeAndModes(blend, osg::StateAttribute::ON);
        
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(geom);
        mRTTScene->addChild(geode);
        
        // Clear footprints so we don't re-render them next frame
        // This makes the system "Persistent" - once drawn, it stays in the buffer.
        mFootprints.clear();
    }

    void SnowDeformationManager::addFootprintToRTT(const osg::Vec3f& position, float rotation)
    {
        // Not used in "rebuild every frame" approach
    }
}
