#include "snowdeformation.hpp"
#include "snowdetection.hpp"
#include "storage.hpp"

#include <components/debug/debuglog.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/shader/shadermanager.hpp>

#include <osg/Geode>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Uniform>
#include <osgDB/WriteFile>

namespace Terrain
{
    SnowDeformationManager::SnowDeformationManager(
        Resource::SceneManager* sceneManager,
        Storage* terrainStorage,
        osg::Group* rootNode)
        : mSceneManager(sceneManager)
        , mTerrainStorage(terrainStorage)
        , mWorldspace(ESM::RefId())
        , mEnabled(true)
        , mActive(false)
        , mCurrentTextureIndex(0)
        , mTextureResolution(512)
        , mWorldTextureRadius(150.0f)
        , mTextureCenter(0.0f, 0.0f)
        , mFootprintRadius(24.0f)
        , mFootprintInterval(2.0f)  // Changed from 15.0 - stamp every 2 units for CONTINUOUS TRAIL (like God of War)
        , mDeformationDepth(8.0f)
        , mLastFootprintPos(0.0f, 0.0f, 0.0f)
        , mTimeSinceLastFootprint(999.0f)  // Start high to stamp immediately
        , mCurrentTime(0.0f)
        , mDebugVisualization(true)  // Enable debug HUD by default
    {
        Log(Debug::Info) << "[SNOW] SnowDeformationManager created";

        // Load snow detection patterns
        SnowDetection::loadSnowPatterns();

        // TODO: Load settings
        // mTextureResolution = Settings::terrain().mSnowDeformationResolution;
        // mWorldTextureRadius = Settings::terrain().mSnowDeformationRadius;
        // mFootprintRadius = Settings::terrain().mSnowFootprintRadius;
        // etc.

        // Setup RTT system
        setupRTT(rootNode);
        createDeformationTextures();
        setupFootprintStamping();
        setupDebugHUD(rootNode);
    }

    SnowDeformationManager::~SnowDeformationManager()
    {
        Log(Debug::Info) << "[SNOW] SnowDeformationManager destroyed";
    }

    void SnowDeformationManager::setupRTT(osg::Group* rootNode)
    {
        // Create RTT camera for rendering footprints to deformation texture
        mRTTCamera = new osg::Camera;
        mRTTCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mRTTCamera->setRenderOrder(osg::Camera::PRE_RENDER);

        // Orthographic projection (top-down view)
        mRTTCamera->setProjectionMatrixAsOrtho(
            -mWorldTextureRadius, mWorldTextureRadius,
            -mWorldTextureRadius, mWorldTextureRadius,
            -100.0f, 100.0f
        );

        // View from above, looking down
        mRTTCamera->setViewMatrixAsLookAt(
            osg::Vec3(0.0f, 100.0f, 0.0f),  // Eye position (above)
            osg::Vec3(0.0f, 0.0f, 0.0f),    // Look at origin
            osg::Vec3(0.0f, 0.0f, 1.0f)     // Up = North (Z axis)
        );

        // Clear to black (no deformation initially)
        mRTTCamera->setClearMask(GL_COLOR_BUFFER_BIT);
        mRTTCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
        mRTTCamera->setViewport(0, 0, mTextureResolution, mTextureResolution);

        // Start disabled
        mRTTCamera->setNodeMask(0);

        // Add to scene
        rootNode->addChild(mRTTCamera);

        Log(Debug::Info) << "[SNOW] RTT camera created: " << mTextureResolution << "x" << mTextureResolution;
    }

    void SnowDeformationManager::createDeformationTextures()
    {
        // Create ping-pong textures for accumulation
        for (int i = 0; i < 2; ++i)
        {
            mDeformationTexture[i] = new osg::Texture2D;
            mDeformationTexture[i]->setTextureSize(mTextureResolution, mTextureResolution);
            mDeformationTexture[i]->setInternalFormat(GL_RGBA16F_ARB);
            mDeformationTexture[i]->setSourceFormat(GL_RGBA);
            mDeformationTexture[i]->setSourceType(GL_FLOAT);
            mDeformationTexture[i]->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
            mDeformationTexture[i]->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
            mDeformationTexture[i]->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            mDeformationTexture[i]->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

            // Allocate texture memory with initial black color
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(mTextureResolution, mTextureResolution, 1, GL_RGBA, GL_FLOAT);
            float* data = reinterpret_cast<float*>(image->data());

            // DEBUG: Fill with test pattern to verify shader is sampling
            // Create a circular depression in the center for testing
            int centerX = mTextureResolution / 2;
            int centerY = mTextureResolution / 2;
            float testRadius = mTextureResolution / 4.0f;

            for (int y = 0; y < mTextureResolution; y++)
            {
                for (int x = 0; x < mTextureResolution; x++)
                {
                    int idx = (y * mTextureResolution + x) * 4; // RGBA

                    // Calculate distance from center
                    float dx = x - centerX;
                    float dy = y - centerY;
                    float dist = std::sqrt(dx*dx + dy*dy);

                    // Create circular depression
                    float depth = 0.0f;
                    if (dist < testRadius)
                    {
                        // Smooth falloff
                        float t = 1.0f - (dist / testRadius);
                        depth = t * t * 50.0f; // 50 units deep at center
                    }

                    data[idx + 0] = depth;  // R = depth
                    data[idx + 1] = 0.0f;   // G = age
                    data[idx + 2] = 0.0f;   // B = unused
                    data[idx + 3] = 1.0f;   // A = 1.0
                }
            }

            Log(Debug::Info) << "[SNOW DEBUG] Created test pattern in deformation texture "
                            << i << " (50 unit deep circle at center)";

            mDeformationTexture[i]->setImage(image);

            // DIAGNOSTIC: Save texture to disk as TGA (PNG doesn't support float textures)
            // Convert float data to visible 8-bit grayscale for debugging
            osg::ref_ptr<osg::Image> debugImage = new osg::Image;
            debugImage->allocateImage(mTextureResolution, mTextureResolution, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE);
            unsigned char* debugData = debugImage->data();

            float maxDepthFound = 0.0f;
            for (int y = 0; y < mTextureResolution; y++)
            {
                for (int x = 0; x < mTextureResolution; x++)
                {
                    int srcIdx = (y * mTextureResolution + x) * 4;
                    int dstIdx = y * mTextureResolution + x;
                    // Scale depth value (0-50) to 0-255 for visibility
                    float depth = data[srcIdx];
                    if (depth > maxDepthFound) maxDepthFound = depth;
                    debugData[dstIdx] = static_cast<unsigned char>(std::min(255.0f, depth * 5.0f));
                }
            }

            Log(Debug::Info) << "[SNOW DEBUG] Test pattern max depth: " << maxDepthFound
                            << " (expected ~50)";

            std::string filename = "deformation_texture_" + std::to_string(i) + "_test_pattern.tga";
            bool success = osgDB::writeImageFile(*debugImage, filename);
            if (success)
            {
                Log(Debug::Info) << "[SNOW DEBUG] Saved test pattern to: " << filename;
            }
            else
            {
                Log(Debug::Warning) << "[SNOW DEBUG] Failed to save test pattern to: " << filename;
            }
        }

        mCurrentTextureIndex = 0;

        // Attach first texture to RTT camera
        mRTTCamera->attach(osg::Camera::COLOR_BUFFER, mDeformationTexture[0].get());

        Log(Debug::Info) << "[SNOW] Deformation textures created (ping-pong)";
    }

    void SnowDeformationManager::setupFootprintStamping()
    {
        // Create a group to hold footprint rendering geometry
        mFootprintGroup = new osg::Group;
        mRTTCamera->addChild(mFootprintGroup);

        // Create a full-screen quad for footprint stamping
        mFootprintQuad = new osg::Geometry;
        mFootprintQuad->setUseDisplayList(false);
        mFootprintQuad->setUseVertexBufferObjects(true);

        // Create vertices for a quad covering the texture
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3(-mWorldTextureRadius, 0.0f, -mWorldTextureRadius));
        vertices->push_back(osg::Vec3( mWorldTextureRadius, 0.0f, -mWorldTextureRadius));
        vertices->push_back(osg::Vec3( mWorldTextureRadius, 0.0f,  mWorldTextureRadius));
        vertices->push_back(osg::Vec3(-mWorldTextureRadius, 0.0f,  mWorldTextureRadius));
        mFootprintQuad->setVertexArray(vertices);

        // UV coordinates
        osg::ref_ptr<osg::Vec2Array> uvs = new osg::Vec2Array;
        uvs->push_back(osg::Vec2(0.0f, 0.0f));
        uvs->push_back(osg::Vec2(1.0f, 0.0f));
        uvs->push_back(osg::Vec2(1.0f, 1.0f));
        uvs->push_back(osg::Vec2(0.0f, 1.0f));
        mFootprintQuad->setTexCoordArray(0, uvs);

        // Primitive
        mFootprintQuad->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));

        // Create state set for footprint rendering
        mFootprintStateSet = new osg::StateSet;

        // Load and attach shaders for footprint stamping
        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->setName("SnowFootprintStamping");

        // Create shaders directly (shader manager integration can come later)
        osg::ref_ptr<osg::Shader> vertShader = new osg::Shader(osg::Shader::VERTEX);
        osg::ref_ptr<osg::Shader> fragShader = new osg::Shader(osg::Shader::FRAGMENT);

        // Load shader source from files
        // For now, we'll use inline shaders to ensure they work
        std::string vertSource = R"(
            #version 120
            uniform vec2 deformationCenter;
            uniform float deformationRadius;
            varying vec2 worldPos;
            varying vec2 texUV;

            void main()
            {
                gl_Position = gl_Vertex;
                worldPos = deformationCenter + gl_Vertex.xy * deformationRadius;
                texUV = gl_MultiTexCoord0.xy;
            }
        )";

        std::string fragSource = R"(
            #version 120
            uniform sampler2D previousDeformation;
            uniform vec2 footprintCenter;
            uniform float footprintRadius;
            uniform float deformationDepth;
            uniform float currentTime;
            varying vec2 worldPos;
            varying vec2 texUV;

            void main()
            {
                vec4 prevDeform = texture2D(previousDeformation, texUV);
                float prevDepth = prevDeform.r;
                float prevAge = prevDeform.g;

                float dist = length(worldPos - footprintCenter);
                float influence = 1.0 - smoothstep(footprintRadius * 0.5, footprintRadius, dist);

                float newDepth = max(prevDepth, influence * deformationDepth);
                float age = (influence > 0.01) ? currentTime : prevAge;

                gl_FragColor = vec4(newDepth, age, 0.0, 1.0);
            }
        )";

        vertShader->setShaderSource(vertSource);
        fragShader->setShaderSource(fragSource);

        program->addShader(vertShader);
        program->addShader(fragShader);
        mFootprintStateSet->setAttributeAndModes(program, osg::StateAttribute::ON);

        // Create uniforms
        mFootprintStateSet->addUniform(new osg::Uniform("previousDeformation", 0));
        mFootprintStateSet->addUniform(new osg::Uniform("deformationCenter", mTextureCenter));
        mFootprintStateSet->addUniform(new osg::Uniform("deformationRadius", mWorldTextureRadius));
        mFootprintStateSet->addUniform(new osg::Uniform("footprintCenter", osg::Vec2f(0.0f, 0.0f)));
        mFootprintStateSet->addUniform(new osg::Uniform("footprintRadius", mFootprintRadius));
        mFootprintStateSet->addUniform(new osg::Uniform("deformationDepth", mDeformationDepth));
        mFootprintStateSet->addUniform(new osg::Uniform("currentTime", 0.0f));

        // Bind previous deformation texture to unit 0
        mFootprintStateSet->setTextureAttributeAndModes(0,
            mDeformationTexture[0].get(), osg::StateAttribute::ON);

        mFootprintQuad->setStateSet(mFootprintStateSet);

        // Add to geode
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(mFootprintQuad);
        mFootprintGroup->addChild(geode);

        // Disable by default
        mFootprintGroup->setNodeMask(0);

        Log(Debug::Info) << "[SNOW] Footprint stamping setup complete with inline shaders";
    }

    void SnowDeformationManager::update(float dt, const osg::Vec3f& playerPos)
    {
        if (!mEnabled)
            return;

        mCurrentTime += dt;

        // Check if we should be active (player on snow)
        bool shouldActivate = shouldBeActive(playerPos);

        if (shouldActivate != mActive)
        {
            mActive = shouldActivate;
            Log(Debug::Info) << "[SNOW] Deformation system " << (mActive ? "activated" : "deactivated");
        }

        if (!mActive)
            return;

        // Update deformation texture center to follow player
        updateCameraPosition(playerPos);

        // Check if player has moved enough for a new footprint
        mTimeSinceLastFootprint += dt;

        float distanceMoved = (playerPos - mLastFootprintPos).length();
        if (distanceMoved > mFootprintInterval || mTimeSinceLastFootprint > 0.5f)
        {
            stampFootprint(playerPos);
            mLastFootprintPos = playerPos;
            mTimeSinceLastFootprint = 0.0f;
        }

        // DIAGNOSTIC: Save texture every 5 seconds to see if it's changing
        static float timeSinceLastSave = 0.0f;
        static int saveCount = 0;
        timeSinceLastSave += dt;
        if (timeSinceLastSave > 5.0f && saveCount < 5)  // Only save 5 times to avoid spam
        {
            osg::Image* img = mDeformationTexture[mCurrentTextureIndex]->getImage();
            if (img && img->getPixelFormat() == GL_RGBA && img->getDataType() == GL_FLOAT)
            {
                // Convert float to grayscale for debugging
                const float* data = reinterpret_cast<const float*>(img->data());
                osg::ref_ptr<osg::Image> debugImage = new osg::Image;
                debugImage->allocateImage(mTextureResolution, mTextureResolution, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE);
                unsigned char* debugData = debugImage->data();

                for (int y = 0; y < mTextureResolution; y++)
                {
                    for (int x = 0; x < mTextureResolution; x++)
                    {
                        int srcIdx = (y * mTextureResolution + x) * 4;
                        int dstIdx = y * mTextureResolution + x;
                        float depth = data[srcIdx];
                        debugData[dstIdx] = static_cast<unsigned char>(std::min(255.0f, depth * 5.0f));
                    }
                }

                std::string filename = "deformation_runtime_" + std::to_string(saveCount) + ".tga";
                if (osgDB::writeImageFile(*debugImage, filename))
                {
                    Log(Debug::Info) << "[SNOW DEBUG] Saved runtime texture to: " << filename
                                    << " at player pos (" << (int)playerPos.x() << ", " << (int)playerPos.y() << ")";
                }
            }
            timeSinceLastSave = 0.0f;
            saveCount++;
        }

        // Update debug HUD
        updateDebugHUD();

        // TODO: Apply decay over time
    }

    bool SnowDeformationManager::shouldBeActive(const osg::Vec3f& worldPos)
    {
        if (!mEnabled)
            return false;

        // Check if player is on snow texture
        bool onSnow = SnowDetection::hasSnowAtPosition(worldPos, mTerrainStorage, mWorldspace);

        // For now, also allow activation if we're in specific test regions
        // TODO: Remove this when hasSnowAtPosition is fully implemented
        // Enable everywhere for testing
        onSnow = true;  // TEMPORARY for testing

        return onSnow;
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
                if (mRTTCamera)
                    mRTTCamera->setNodeMask(0);
            }
        }
    }

    void SnowDeformationManager::setWorldspace(ESM::RefId worldspace)
    {
        mWorldspace = worldspace;
    }

    osg::Texture2D* SnowDeformationManager::getDeformationTexture() const
    {
        if (!mActive || !mEnabled)
            return nullptr;

        return mDeformationTexture[mCurrentTextureIndex].get();
    }

    void SnowDeformationManager::getDeformationTextureParams(
        osg::Vec2f& outCenter, float& outRadius) const
    {
        outCenter = mTextureCenter;
        outRadius = mWorldTextureRadius;
    }

    void SnowDeformationManager::updateCameraPosition(const osg::Vec3f& playerPos)
    {
        // Update texture center to player's XY position (ground plane)
        // OpenMW: X=East, Y=North, Z=Up
        mTextureCenter.set(playerPos.x(), playerPos.y());

        static int logCount = 0;
        if (logCount++ < 3)
        {
            Log(Debug::Info) << "[SNOW CAMERA] Player at (" << (int)playerPos.x()
                            << ", " << (int)playerPos.y() << ", " << (int)playerPos.z() << ")"
                            << " → TextureCenter=(" << (int)mTextureCenter.x()
                            << ", " << (int)mTextureCenter.y() << ")"
                            << " [Fixed: using XY not XZ]";
        }

        // Move RTT camera to center over player
        // Camera looks down from above (Z+) onto the XY ground plane
        if (mRTTCamera)
        {
            mRTTCamera->setViewMatrixAsLookAt(
                osg::Vec3(playerPos.x(), playerPos.y(), playerPos.z() + 100.0f),  // Eye 100 units above player
                osg::Vec3(playerPos.x(), playerPos.y(), playerPos.z()),            // Look at player position
                osg::Vec3(0.0f, 1.0f, 0.0f)                                        // Up = North (Y axis)
            );
        }
    }

    void SnowDeformationManager::stampFootprint(const osg::Vec3f& position)
    {
        if (!mFootprintStateSet || !mRTTCamera)
            return;

        Log(Debug::Info) << "[SNOW] Stamping footprint at "
                        << (int)position.x() << ", " << (int)position.y();

        // Swap ping-pong buffers
        int prevIndex = mCurrentTextureIndex;
        mCurrentTextureIndex = 1 - mCurrentTextureIndex;

        // Bind previous texture as input (texture unit 0)
        mFootprintStateSet->setTextureAttributeAndModes(0,
            mDeformationTexture[prevIndex].get(),
            osg::StateAttribute::ON);

        // Attach current texture as render target
        mRTTCamera->detach(osg::Camera::COLOR_BUFFER);
        mRTTCamera->attach(osg::Camera::COLOR_BUFFER,
            mDeformationTexture[mCurrentTextureIndex].get());

        // Update shader uniforms - use XY not XZ!
        osg::Uniform* footprintCenterUniform = mFootprintStateSet->getUniform("footprintCenter");
        if (footprintCenterUniform)
            footprintCenterUniform->set(osg::Vec2f(position.x(), position.y()));

        osg::Uniform* deformationCenterUniform = mFootprintStateSet->getUniform("deformationCenter");
        if (deformationCenterUniform)
            deformationCenterUniform->set(mTextureCenter);

        osg::Uniform* currentTimeUniform = mFootprintStateSet->getUniform("currentTime");
        if (currentTimeUniform)
            currentTimeUniform->set(mCurrentTime);

        // DISABLED FOR TESTING: Keep RTT camera off to verify static test pattern works
        // The test pattern in the texture should be visible without RTT rendering
        // TODO: Re-enable once we confirm static deformation works
        // mRTTCamera->setNodeMask(~0u);
        // mFootprintGroup->setNodeMask(~0u);

        Log(Debug::Info) << "[SNOW] Footprint stamping DISABLED for testing";
    }

    void SnowDeformationManager::setupDebugHUD(osg::Group* rootNode)
    {
        // Create HUD camera that renders on top of everything
        mDebugHUDCamera = new osg::Camera;
        mDebugHUDCamera->setRenderOrder(osg::Camera::POST_RENDER);
        mDebugHUDCamera->setClearMask(0);  // Don't clear anything
        mDebugHUDCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mDebugHUDCamera->setViewMatrix(osg::Matrix::identity());

        // Use normalized coordinates (0-1) so it works at any resolution
        mDebugHUDCamera->setProjectionMatrixAsOrtho2D(0, 1, 0, 1);

        // Create quad in top-right corner (20% of screen size)
        float hudSize = 0.2f;  // 20% of screen
        float margin = 0.01f;  // 1% margin
        float left = 1.0f - hudSize - margin;
        float bottom = 1.0f - hudSize - margin;
        float right = 1.0f - margin;
        float top = 1.0f - margin;

        mDebugQuad = new osg::Geometry;
        mDebugQuad->setUseDisplayList(false);
        mDebugQuad->setUseVertexBufferObjects(true);

        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3(left, bottom, 0.0f));
        vertices->push_back(osg::Vec3(right, bottom, 0.0f));
        vertices->push_back(osg::Vec3(right, top, 0.0f));
        vertices->push_back(osg::Vec3(left, top, 0.0f));
        mDebugQuad->setVertexArray(vertices);

        osg::ref_ptr<osg::Vec2Array> uvs = new osg::Vec2Array;
        uvs->push_back(osg::Vec2(0.0f, 0.0f));
        uvs->push_back(osg::Vec2(1.0f, 0.0f));
        uvs->push_back(osg::Vec2(1.0f, 1.0f));
        uvs->push_back(osg::Vec2(0.0f, 1.0f));
        mDebugQuad->setTexCoordArray(0, uvs);

        mDebugQuad->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));

        // Create state set with simple texture display shader
        osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet;

        // Create simple passthrough shader
        osg::ref_ptr<osg::Program> program = new osg::Program;

        std::string vertSource = R"(
            #version 120
            varying vec2 texCoord;
            void main()
            {
                gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
                texCoord = gl_MultiTexCoord0.xy;
            }
        )";

        std::string fragSource = R"(
            #version 120
            uniform sampler2D debugTexture;
            varying vec2 texCoord;
            void main()
            {
                vec4 texColor = texture2D(debugTexture, texCoord);
                // Visualize the depth channel (R) as grayscale, scaled for visibility
                float depth = texColor.r;
                // Scale by 10 to make small deformations visible (50 units -> 0.5 brightness)
                float brightness = depth * 0.1;
                gl_FragColor = vec4(brightness, brightness, brightness, 1.0);

                // Add a colored border to make it obvious
                if (texCoord.x < 0.02 || texCoord.x > 0.98 ||
                    texCoord.y < 0.02 || texCoord.y > 0.98)
                {
                    gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);  // Red border
                }
            }
        )";

        osg::ref_ptr<osg::Shader> vertShader = new osg::Shader(osg::Shader::VERTEX, vertSource);
        osg::ref_ptr<osg::Shader> fragShader = new osg::Shader(osg::Shader::FRAGMENT, fragSource);
        program->addShader(vertShader);
        program->addShader(fragShader);

        stateSet->setAttributeAndModes(program, osg::StateAttribute::ON);
        stateSet->addUniform(new osg::Uniform("debugTexture", 0));

        // Bind the deformation texture
        stateSet->setTextureAttributeAndModes(0, mDeformationTexture[0].get(), osg::StateAttribute::ON);

        // Disable depth test so it draws on top
        stateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        stateSet->setRenderBinDetails(1000, "RenderBin");  // Render last

        mDebugQuad->setStateSet(stateSet);

        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(mDebugQuad);
        mDebugHUDCamera->addChild(geode);

        // Start enabled
        mDebugHUDCamera->setNodeMask(mDebugVisualization ? ~0u : 0);

        rootNode->addChild(mDebugHUDCamera);

        Log(Debug::Info) << "[SNOW DEBUG] HUD overlay created (top-right corner, 20% screen size, RED BORDER)";
    }

    void SnowDeformationManager::updateDebugHUD()
    {
        if (!mDebugQuad || !mDebugVisualization)
            return;

        // Update the texture binding to show the current deformation texture
        osg::StateSet* stateSet = mDebugQuad->getStateSet();
        if (stateSet)
        {
            stateSet->setTextureAttributeAndModes(0,
                mDeformationTexture[mCurrentTextureIndex].get(),
                osg::StateAttribute::ON);
        }
    }

    void SnowDeformationManager::setDebugVisualization(bool enabled)
    {
        mDebugVisualization = enabled;
        if (mDebugHUDCamera)
        {
            mDebugHUDCamera->setNodeMask(enabled ? ~0u : 0);
            Log(Debug::Info) << "[SNOW DEBUG] HUD visualization "
                            << (enabled ? "enabled" : "disabled");
        }
    }
}
