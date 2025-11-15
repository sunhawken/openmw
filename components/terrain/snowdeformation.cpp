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
        // Update texture center to player's XZ position
        mTextureCenter.set(playerPos.x(), playerPos.z());

        static int logCount = 0;
        if (logCount++ < 3)
        {
            Log(Debug::Info) << "[SNOW CAMERA] Player at (" << (int)playerPos.x()
                            << ", " << (int)playerPos.y() << ", " << (int)playerPos.z() << ")"
                            << " → TextureCenter=(" << (int)mTextureCenter.x()
                            << ", " << (int)mTextureCenter.y() << ")";
        }

        // Move RTT camera to center over player
        if (mRTTCamera)
        {
            mRTTCamera->setViewMatrixAsLookAt(
                osg::Vec3(playerPos.x(), playerPos.y() + 100.0f, playerPos.z()),  // Eye above player
                osg::Vec3(playerPos.x(), playerPos.y(), playerPos.z()),            // Look at player
                osg::Vec3(0.0f, 0.0f, 1.0f)                                        // Up = North
            );
        }
    }

    void SnowDeformationManager::stampFootprint(const osg::Vec3f& position)
    {
        if (!mFootprintStateSet || !mRTTCamera)
            return;

        Log(Debug::Info) << "[SNOW] Stamping footprint at "
                        << (int)position.x() << ", " << (int)position.z();

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

        // Update shader uniforms
        osg::Uniform* footprintCenterUniform = mFootprintStateSet->getUniform("footprintCenter");
        if (footprintCenterUniform)
            footprintCenterUniform->set(osg::Vec2f(position.x(), position.z()));

        osg::Uniform* deformationCenterUniform = mFootprintStateSet->getUniform("deformationCenter");
        if (deformationCenterUniform)
            deformationCenterUniform->set(mTextureCenter);

        osg::Uniform* currentTimeUniform = mFootprintStateSet->getUniform("currentTime");
        if (currentTimeUniform)
            currentTimeUniform->set(mCurrentTime);

        // Enable RTT camera and footprint group for this frame
        mRTTCamera->setNodeMask(~0u);
        mFootprintGroup->setNodeMask(~0u);

        // NOTE: The rendering will happen automatically in the next frame
        // The node masks will be disabled after integration with update loop
    }
}
