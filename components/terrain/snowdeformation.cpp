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
        , mTexturesInitialized(false)
        , mTextureResolution(1024)  // Increased from 512 for better quality
        , mWorldTextureRadius(300.0f)  // Increased from 150 for larger coverage
        , mTextureCenter(0.0f, 0.0f)
        , mFootprintRadius(60.0f)  // Default for snow (wide, body-sized), updated per-terrain
        , mFootprintInterval(2.0f)  // Default, will be updated per-terrain
        , mDeformationDepth(100.0f)  // Default for snow (waist-deep), updated per-terrain - MUST match snowRaiseAmount in shader!
        , mLastFootprintPos(0.0f, 0.0f, 0.0f)
        , mCurrentPlayerPos(0.0f, 0.0f, 0.0f)  // Will be updated in first update() call
        , mTimeSinceLastFootprint(999.0f)  // Start high to stamp immediately
        , mLastBlitCenter(0.0f, 0.0f)
        , mBlitThreshold(50.0f)  // Blit when player moves 50+ units
        , mDecayTime(120.0f)  // 2 minutes for full restoration
        , mTimeSinceLastDecay(0.0f)
        , mDecayUpdateInterval(0.1f)  // Apply decay every 0.1 seconds
        , mCurrentTerrainType("snow")
        , mCurrentTime(0.0f)
    {
        Log(Debug::Info) << "[SNOW] SnowDeformationManager created";

        // Load snow detection patterns
        SnowDetection::loadSnowPatterns();

        // Initialize terrain-based parameters
        // IMPORTANT: Depth is passed to shader as snowRaiseAmount uniform
        // Terrain raised by 'depth' units, footprints dig down 'depth' units to ground level
        mTerrainParams = {
            {60.0f, 100.0f, 2.0f, "snow"},   // Snow: wide radius (body), waist-deep (100 units), frequent stamps
            {30.0f, 60.0f, 3.0f, "ash"},     // Ash: medium radius, knee-deep (60 units)
            {15.0f, 30.0f, 5.0f, "mud"},     // Mud: narrow radius (feet only), ankle-deep (30 units)
            {20.0f, 40.0f, 4.0f, "dirt"},    // Dirt: similar to mud
            {25.0f, 50.0f, 3.5f, "sand"}     // Sand: between ash and mud
        };

        // TODO: Load settings
        // mTextureResolution = Settings::terrain().mSnowDeformationResolution;
        // mWorldTextureRadius = Settings::terrain().mSnowDeformationRadius;
        // etc.

        // Setup RTT system
        setupRTT(rootNode);
        createDeformationTextures();
        setupFootprintStamping();
        setupBlitSystem();
        setupDecaySystem();

        // Initialize blit center to current position (will be updated on first frame)
        mLastBlitCenter = mTextureCenter;

        Log(Debug::Info) << "[SNOW] All deformation systems initialized";
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

        // CRITICAL: Set reference frame to ABSOLUTE_RF so camera uses its own view/projection
        // matrices and ignores the parent's transforms
        // This is standard for RTT cameras
        mRTTCamera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);

        // Orthographic projection (top-down view)
        // CRITICAL FIX: Expanded near/far planes to cover all terrain heights
        // Previous -100/+100 was too narrow and clipped terrain
        mRTTCamera->setProjectionMatrixAsOrtho(
            -mWorldTextureRadius, mWorldTextureRadius,
            -mWorldTextureRadius, mWorldTextureRadius,
            -10000.0f, 10000.0f  // Wide range to capture all terrain
        );

        // View from above, looking down
        // CRITICAL: In OpenMW Z is up, so camera at Z=100 looks down at Z=0
        // The "up" vector must be in the ground plane (XY plane) - using negative Y (South)
        // This ensures the rendered texture has North at top, South at bottom
        mRTTCamera->setViewMatrixAsLookAt(
            osg::Vec3(0.0f, 0.0f, 100.0f),  // Eye position (100 units ABOVE in Z)
            osg::Vec3(0.0f, 0.0f, 0.0f),    // Look at origin (down Z-axis)
            osg::Vec3(0.0f, -1.0f, 0.0f)    // Up = -Y (South), so +Y (North) is at top of texture
        );

        // ENABLE clearing - the ping-pong shader handles accumulation
        // by reading from previous texture and writing to current
        // Clearing ensures we start fresh each frame with the shader's output
        mRTTCamera->setClearMask(GL_COLOR_BUFFER_BIT);
        mRTTCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        mRTTCamera->setViewport(0, 0, mTextureResolution, mTextureResolution);

        // Start disabled
        mRTTCamera->setNodeMask(0);

        // Add to scene
        rootNode->addChild(mRTTCamera);

        Log(Debug::Info) << "[SNOW] RTT camera created: " << mTextureResolution << "x" << mTextureResolution
                        << " FBO implementation=" << (mRTTCamera->getRenderTargetImplementation() == osg::Camera::FRAME_BUFFER_OBJECT)
                        << " Render order=" << mRTTCamera->getRenderOrder()
                        << " Clear mask=" << mRTTCamera->getClearMask()
                        << " Initial node mask=" << mRTTCamera->getNodeMask()
                        << " Parent=" << (rootNode != nullptr);
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

            // CRITICAL FIX: Do NOT set an Image on RTT textures!
            // RTT textures are GPU-only. Setting an Image prevents RTT from working.
            // The texture will be initialized by the first clear operation from the RTT camera.
            //
            // Previously we were doing:
            //   mDeformationTexture[i]->setImage(image);
            // This caused the texture to have a CPU-side image that OSG would never update
            // from the GPU render target, resulting in 0-byte saves.
            //
            // Instead, rely on the RTT camera's clear to initialize to zero.

            Log(Debug::Info) << "[SNOW] Created deformation texture " << i
                            << " (" << mTextureResolution << "x" << mTextureResolution << ")";
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
        // CRITICAL: In OpenMW, Z is up, so the ground plane is X-Y
        // The quad must be in the X-Y plane (at Z=0 in local camera space)
        // The camera will be positioned at player's altitude, so Z=0 here is correct
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3(-mWorldTextureRadius, -mWorldTextureRadius, 0.0f));  // Bottom-left
        vertices->push_back(osg::Vec3( mWorldTextureRadius, -mWorldTextureRadius, 0.0f));  // Bottom-right
        vertices->push_back(osg::Vec3( mWorldTextureRadius,  mWorldTextureRadius, 0.0f));  // Top-right
        vertices->push_back(osg::Vec3(-mWorldTextureRadius,  mWorldTextureRadius, 0.0f));  // Top-left
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
            varying vec2 texUV;

            void main()
            {
                // Transform vertex through RTT camera's projection/view matrices
                // The quad covers the entire deformation texture area in world space
                gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;

                // UV coordinates for sampling previous deformation texture
                texUV = gl_MultiTexCoord0.xy;
            }
        )";

        std::string fragSource = R"(
            #version 120
            uniform sampler2D previousDeformation;
            uniform vec2 deformationCenter;      // World XY center of texture
            uniform float deformationRadius;     // World radius covered by texture
            uniform vec2 footprintCenter;        // World XY position of new footprint
            uniform float footprintRadius;       // World radius of footprint
            uniform float deformationDepth;      // Maximum depth in world units
            uniform float currentTime;           // Current game time
            varying vec2 texUV;

            void main()
            {
                // Sample previous deformation at this UV
                vec4 prevDeform = texture2D(previousDeformation, texUV);
                float prevDepth = prevDeform.r;
                float prevAge = prevDeform.g;

                // Convert UV (0-1) to world position
                // UV (0,0) = bottom-left, UV (1,1) = top-right
                vec2 worldPos = deformationCenter + (texUV - 0.5) * 2.0 * deformationRadius;

                // Calculate distance from footprint center
                float dist = length(worldPos - footprintCenter);

                // Circular falloff: full depth at center, fades to zero at radius
                float influence = 1.0 - smoothstep(footprintRadius * 0.5, footprintRadius, dist);

                // Accumulate deformation (keep maximum depth)
                float newDepth = max(prevDepth, influence * deformationDepth);

                // Update age where new footprint is stamped
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

        Log(Debug::Info) << "[SNOW] Footprint stamping setup complete with inline shaders"
                        << " Quad vertices=" << vertices->size()
                        << " Quad UVs=" << uvs->size()
                        << " State set valid=" << (mFootprintStateSet.valid())
                        << " Footprint group children=" << mFootprintGroup->getNumChildren()
                        << " RTT camera children=" << mRTTCamera->getNumChildren();
    }

    void SnowDeformationManager::update(float dt, const osg::Vec3f& playerPos)
    {
        if (!mEnabled)
            return;

        mCurrentTime += dt;
        mCurrentPlayerPos = playerPos;  // Track current player position for quad updates

        // Check if we should be active (player on snow)
        bool shouldActivate = shouldBeActive(playerPos);

        if (shouldActivate != mActive)
        {
            mActive = shouldActivate;
            Log(Debug::Info) << "[SNOW] Deformation system " << (mActive ? "activated" : "deactivated");
        }

        if (!mActive)
            return;

        // CRITICAL: Initialize textures on first activation
        // Since we don't use setImage(), both textures need to be cleared via RTT
        // This will happen naturally during the first few render frames
        if (!mTexturesInitialized)
        {
            Log(Debug::Info) << "[SNOW] First activation - textures will be initialized by RTT clear";
            mTexturesInitialized = true;
        }

        // Disable all RTT groups from previous frame (cleanup)
        // Each frame, we'll enable only the one operation we need
        if (mBlitGroup)
            mBlitGroup->setNodeMask(0);
        if (mFootprintGroup)
            mFootprintGroup->setNodeMask(0);
        if (mDecayGroup)
            mDecayGroup->setNodeMask(0);

        // Update terrain-specific parameters based on current terrain texture
        updateTerrainParameters(playerPos);

        // IMPORTANT: We can only do ONE RTT operation per frame to avoid conflicts
        // Priority: blit > footprint > decay

        // Check if we need to blit (texture recenter)
        osg::Vec2f currentCenter(playerPos.x(), playerPos.y());
        float distanceFromLastBlit = (currentCenter - mLastBlitCenter).length();

        if (distanceFromLastBlit > mBlitThreshold)
        {
            // Blit old texture to new position before recentering
            blitTexture(mTextureCenter, currentCenter);
            mLastBlitCenter = currentCenter;

            // Update camera position after blit
            updateCameraPosition(playerPos);

            // Skip footprint and decay this frame - blit has priority
            return;
        }

        // Update deformation texture center to follow player (smooth following)
        updateCameraPosition(playerPos);

        // Check if player has moved enough for a new footprint
        mTimeSinceLastFootprint += dt;

        float distanceMoved = (playerPos - mLastFootprintPos).length();

        // DIAGNOSTIC: Log first few movement checks
        static int moveCheckCount = 0;
        if (moveCheckCount++ < 10)
        {
            Log(Debug::Info) << "[SNOW UPDATE] distanceMoved=" << distanceMoved
                            << " footprintInterval=" << mFootprintInterval
                            << " timeSinceLast=" << mTimeSinceLastFootprint
                            << " willStamp=" << (distanceMoved > mFootprintInterval || mTimeSinceLastFootprint > 0.5f);
        }

        if (distanceMoved > mFootprintInterval || mTimeSinceLastFootprint > 0.5f)
        {
            stampFootprint(playerPos);
            mLastFootprintPos = playerPos;
            mTimeSinceLastFootprint = 0.0f;

            // Skip decay this frame - footprint has priority
            return;
        }

        // Apply decay periodically (lowest priority)
        mTimeSinceLastDecay += dt;
        if (mTimeSinceLastDecay > mDecayUpdateInterval)
        {
            applyDecay(mTimeSinceLastDecay);
            mTimeSinceLastDecay = 0.0f;
        }
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
        // CRITICAL: OpenMW coordinate system
        // X = East/West, Y = North/South, Z = Up/Down (altitude)
        // Texture center should follow player on the GROUND PLANE (X and Y), not altitude (Z)
        mTextureCenter.set(playerPos.x(), playerPos.y());  // Use XY (ground plane)

        static int logCount = 0;
        if (logCount++ < 3)
        {
            Log(Debug::Info) << "[SNOW CAMERA] Player at (" << (int)playerPos.x()
                            << ", " << (int)playerPos.y() << ", " << (int)playerPos.z() << ")"
                            << " → TextureCenter=(" << (int)mTextureCenter.x()
                            << ", " << (int)mTextureCenter.y() << ")"
                            << " [Using XY ground plane, Z=" << (int)playerPos.z() << " is altitude]";
        }

        // Move RTT camera to center over player
        // Camera looks down from above (Z+) onto the XY ground plane
        if (mRTTCamera)
        {
            mRTTCamera->setViewMatrixAsLookAt(
                osg::Vec3(playerPos.x(), playerPos.y(), playerPos.z() + 100.0f),  // Eye 100 units above player (Z+ is up)
                osg::Vec3(playerPos.x(), playerPos.y(), playerPos.z()),            // Look at player position
                osg::Vec3(0.0f, -1.0f, 0.0f)                                       // Up = -Y (South), matching setupRTT
            );
        }
    }

    void SnowDeformationManager::stampFootprint(const osg::Vec3f& position)
    {
        if (!mFootprintStateSet || !mRTTCamera || !mFootprintQuad)
            return;

        Log(Debug::Info) << "[SNOW] Stamping footprint at "
                        << (int)position.x() << ", " << (int)position.y()
                        << " with depth=" << mDeformationDepth
                        << ", radius=" << mFootprintRadius;

        // CRITICAL FIX: Update quad Z position to match current terrain/player altitude
        // The quad must be within the camera's view frustum to render
        // With camera looking down from playerZ+100, quad should be near playerZ
        osg::Vec3Array* vertices = static_cast<osg::Vec3Array*>(mFootprintQuad->getVertexArray());
        if (vertices && vertices->size() == 4)
        {
            // Set all vertices to player's current altitude (Z position)
            // Keep XY extents the same (covering the deformation radius)
            for (unsigned int i = 0; i < 4; ++i)
            {
                (*vertices)[i].z() = position.z();  // Match player altitude
            }
            vertices->dirty();  // Mark as modified

            static int logCount = 0;
            if (logCount++ < 3)
            {
                Log(Debug::Info) << "[SNOW FIX] Updated footprint quad Z to " << position.z()
                                << " (player altitude)";
            }
        }

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

        // Update shader uniforms (IMPORTANT: update terrain-specific parameters!)
        osg::Uniform* footprintCenterUniform = mFootprintStateSet->getUniform("footprintCenter");
        if (footprintCenterUniform)
            footprintCenterUniform->set(osg::Vec2f(position.x(), position.y()));

        osg::Uniform* deformationCenterUniform = mFootprintStateSet->getUniform("deformationCenter");
        if (deformationCenterUniform)
            deformationCenterUniform->set(mTextureCenter);

        osg::Uniform* currentTimeUniform = mFootprintStateSet->getUniform("currentTime");
        if (currentTimeUniform)
            currentTimeUniform->set(mCurrentTime);

        // UPDATE terrain-specific parameters that may have changed
        osg::Uniform* deformationDepthUniform = mFootprintStateSet->getUniform("deformationDepth");
        if (deformationDepthUniform)
            deformationDepthUniform->set(mDeformationDepth);

        osg::Uniform* footprintRadiusUniform = mFootprintStateSet->getUniform("footprintRadius");
        if (footprintRadiusUniform)
            footprintRadiusUniform->set(mFootprintRadius);

        // Enable RTT rendering to stamp footprint
        mRTTCamera->setNodeMask(~0u);
        mFootprintGroup->setNodeMask(~0u);

        // DIAGNOSTIC: Save texture after first few footprints to verify RTT is working
        static int stampCount = 0;
        stampCount++;

        Log(Debug::Info) << "[SNOW] Footprint stamped, count=" << stampCount
                        << " RTT camera enabled=" << (mRTTCamera->getNodeMask() != 0)
                        << " Footprint group enabled=" << (mFootprintGroup->getNodeMask() != 0)
                        << " Current texture index=" << mCurrentTextureIndex;

        // DIAGNOSTIC CALLBACK DISABLED: Was causing crashes when reading GPU framebuffer
        // The RTT camera is working correctly (footprints are being stamped)
        // To verify RTT output, use shader diagnostic tests in terrain.vert instead
    }

    void SnowDeformationManager::setupBlitSystem()
    {
        // Create blit group and quad for texture scrolling
        mBlitGroup = new osg::Group;
        mRTTCamera->addChild(mBlitGroup);

        mBlitQuad = new osg::Geometry;
        mBlitQuad->setUseDisplayList(false);
        mBlitQuad->setUseVertexBufferObjects(true);

        // Full-screen quad in X-Y plane (Z is up in OpenMW)
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3(-mWorldTextureRadius, -mWorldTextureRadius, 0.0f));
        vertices->push_back(osg::Vec3( mWorldTextureRadius, -mWorldTextureRadius, 0.0f));
        vertices->push_back(osg::Vec3( mWorldTextureRadius,  mWorldTextureRadius, 0.0f));
        vertices->push_back(osg::Vec3(-mWorldTextureRadius,  mWorldTextureRadius, 0.0f));
        mBlitQuad->setVertexArray(vertices);

        osg::ref_ptr<osg::Vec2Array> uvs = new osg::Vec2Array;
        uvs->push_back(osg::Vec2(0.0f, 0.0f));
        uvs->push_back(osg::Vec2(1.0f, 0.0f));
        uvs->push_back(osg::Vec2(1.0f, 1.0f));
        uvs->push_back(osg::Vec2(0.0f, 1.0f));
        mBlitQuad->setTexCoordArray(0, uvs);

        mBlitQuad->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));

        // Create blit shader
        mBlitStateSet = new osg::StateSet;
        osg::ref_ptr<osg::Program> program = new osg::Program;

        std::string vertSource = R"(
            #version 120
            varying vec2 texUV;
            void main()
            {
                gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
                texUV = gl_MultiTexCoord0.xy;
            }
        )";

        std::string fragSource = R"(
            #version 120
            uniform sampler2D sourceTexture;
            uniform vec2 oldCenter;
            uniform vec2 newCenter;
            uniform float textureRadius;
            varying vec2 texUV;

            void main()
            {
                // Calculate world position for this UV in the NEW coordinate system
                vec2 worldPos = newCenter + (texUV - 0.5) * 2.0 * textureRadius;

                // Calculate UV in the OLD coordinate system
                vec2 oldUV = ((worldPos - oldCenter) / textureRadius) * 0.5 + 0.5;

                // Sample from old texture if UV is valid, otherwise zero
                if (oldUV.x >= 0.0 && oldUV.x <= 1.0 && oldUV.y >= 0.0 && oldUV.y <= 1.0)
                {
                    gl_FragColor = texture2D(sourceTexture, oldUV);
                }
                else
                {
                    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);  // No deformation
                }
            }
        )";

        osg::ref_ptr<osg::Shader> vertShader = new osg::Shader(osg::Shader::VERTEX, vertSource);
        osg::ref_ptr<osg::Shader> fragShader = new osg::Shader(osg::Shader::FRAGMENT, fragSource);
        program->addShader(vertShader);
        program->addShader(fragShader);
        mBlitStateSet->setAttributeAndModes(program, osg::StateAttribute::ON);

        mBlitStateSet->addUniform(new osg::Uniform("sourceTexture", 0));
        mBlitStateSet->addUniform(new osg::Uniform("oldCenter", osg::Vec2f(0.0f, 0.0f)));
        mBlitStateSet->addUniform(new osg::Uniform("newCenter", osg::Vec2f(0.0f, 0.0f)));
        mBlitStateSet->addUniform(new osg::Uniform("textureRadius", mWorldTextureRadius));

        mBlitQuad->setStateSet(mBlitStateSet);

        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(mBlitQuad);
        mBlitGroup->addChild(geode);

        // Start disabled
        mBlitGroup->setNodeMask(0);

        Log(Debug::Info) << "[SNOW] Blit system setup complete";
    }

    void SnowDeformationManager::setupDecaySystem()
    {
        // Create decay group and quad
        mDecayGroup = new osg::Group;
        mRTTCamera->addChild(mDecayGroup);

        mDecayQuad = new osg::Geometry;
        mDecayQuad->setUseDisplayList(false);
        mDecayQuad->setUseVertexBufferObjects(true);

        // Full-screen quad in X-Y plane (Z is up in OpenMW)
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3(-mWorldTextureRadius, -mWorldTextureRadius, 0.0f));
        vertices->push_back(osg::Vec3( mWorldTextureRadius, -mWorldTextureRadius, 0.0f));
        vertices->push_back(osg::Vec3( mWorldTextureRadius,  mWorldTextureRadius, 0.0f));
        vertices->push_back(osg::Vec3(-mWorldTextureRadius,  mWorldTextureRadius, 0.0f));
        mDecayQuad->setVertexArray(vertices);

        osg::ref_ptr<osg::Vec2Array> uvs = new osg::Vec2Array;
        uvs->push_back(osg::Vec2(0.0f, 0.0f));
        uvs->push_back(osg::Vec2(1.0f, 0.0f));
        uvs->push_back(osg::Vec2(1.0f, 1.0f));
        uvs->push_back(osg::Vec2(0.0f, 1.0f));
        mDecayQuad->setTexCoordArray(0, uvs);

        mDecayQuad->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));

        // Create decay shader
        mDecayStateSet = new osg::StateSet;
        osg::ref_ptr<osg::Program> program = new osg::Program;

        std::string vertSource = R"(
            #version 120
            varying vec2 texUV;
            void main()
            {
                gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
                texUV = gl_MultiTexCoord0.xy;
            }
        )";

        std::string fragSource = R"(
            #version 120
            uniform sampler2D currentDeformation;
            uniform float currentTime;
            uniform float decayTime;
            varying vec2 texUV;

            void main()
            {
                vec4 deform = texture2D(currentDeformation, texUV);
                float depth = deform.r;
                float age = deform.g;

                if (depth > 0.01)
                {
                    // Calculate how long ago this deformation was created
                    float timeSinceCreation = currentTime - age;

                    // Linear decay over decayTime seconds
                    float decayFactor = clamp(timeSinceCreation / decayTime, 0.0, 1.0);
                    depth *= (1.0 - decayFactor);

                    // If depth is very small, zero it out
                    if (depth < 0.01)
                        depth = 0.0;
                }

                gl_FragColor = vec4(depth, age, 0.0, 1.0);
            }
        )";

        osg::ref_ptr<osg::Shader> vertShader = new osg::Shader(osg::Shader::VERTEX, vertSource);
        osg::ref_ptr<osg::Shader> fragShader = new osg::Shader(osg::Shader::FRAGMENT, fragSource);
        program->addShader(vertShader);
        program->addShader(fragShader);
        mDecayStateSet->setAttributeAndModes(program, osg::StateAttribute::ON);

        mDecayStateSet->addUniform(new osg::Uniform("currentDeformation", 0));
        mDecayStateSet->addUniform(new osg::Uniform("currentTime", 0.0f));
        mDecayStateSet->addUniform(new osg::Uniform("decayTime", mDecayTime));

        mDecayQuad->setStateSet(mDecayStateSet);

        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(mDecayQuad);
        mDecayGroup->addChild(geode);

        // Start disabled
        mDecayGroup->setNodeMask(0);

        Log(Debug::Info) << "[SNOW] Decay system setup complete (decay time: " << mDecayTime << "s)";
    }

    void SnowDeformationManager::blitTexture(const osg::Vec2f& oldCenter, const osg::Vec2f& newCenter)
    {
        if (!mBlitStateSet || !mRTTCamera || !mBlitQuad)
            return;

        Log(Debug::Info) << "[SNOW] Blitting texture from (" << (int)oldCenter.x() << ", " << (int)oldCenter.y()
                        << ") to (" << (int)newCenter.x() << ", " << (int)newCenter.y() << ")";

        // Update blit quad Z position to match current altitude
        // (Same as footprint quad fix)
        osg::Vec3Array* vertices = static_cast<osg::Vec3Array*>(mBlitQuad->getVertexArray());
        if (vertices && vertices->size() == 4)
        {
            for (unsigned int i = 0; i < 4; ++i)
            {
                (*vertices)[i].z() = mCurrentPlayerPos.z();  // Match current player altitude
            }
            vertices->dirty();
        }

        // Swap ping-pong buffers
        int sourceIndex = mCurrentTextureIndex;
        mCurrentTextureIndex = 1 - mCurrentTextureIndex;

        // Bind source texture
        mBlitStateSet->setTextureAttributeAndModes(0,
            mDeformationTexture[sourceIndex].get(),
            osg::StateAttribute::ON);

        // Attach destination texture as render target
        mRTTCamera->detach(osg::Camera::COLOR_BUFFER);
        mRTTCamera->attach(osg::Camera::COLOR_BUFFER,
            mDeformationTexture[mCurrentTextureIndex].get());

        // Update shader uniforms
        osg::Uniform* oldCenterUniform = mBlitStateSet->getUniform("oldCenter");
        if (oldCenterUniform)
            oldCenterUniform->set(oldCenter);

        osg::Uniform* newCenterUniform = mBlitStateSet->getUniform("newCenter");
        if (newCenterUniform)
            newCenterUniform->set(newCenter);

        // Enable blit rendering for this frame
        mBlitGroup->setNodeMask(~0u);
        mRTTCamera->setNodeMask(~0u);

        // Other groups already disabled at start of update()
    }

    void SnowDeformationManager::applyDecay(float dt)
    {
        if (!mDecayStateSet || !mRTTCamera || !mDecayQuad)
            return;

        // Update decay quad Z position to match current altitude
        osg::Vec3Array* vertices = static_cast<osg::Vec3Array*>(mDecayQuad->getVertexArray());
        if (vertices && vertices->size() == 4)
        {
            for (unsigned int i = 0; i < 4; ++i)
            {
                (*vertices)[i].z() = mCurrentPlayerPos.z();  // Match current player altitude
            }
            vertices->dirty();
        }

        // Swap ping-pong buffers
        int sourceIndex = mCurrentTextureIndex;
        mCurrentTextureIndex = 1 - mCurrentTextureIndex;

        // Bind source texture
        mDecayStateSet->setTextureAttributeAndModes(0,
            mDeformationTexture[sourceIndex].get(),
            osg::StateAttribute::ON);

        // Attach destination texture as render target
        mRTTCamera->detach(osg::Camera::COLOR_BUFFER);
        mRTTCamera->attach(osg::Camera::COLOR_BUFFER,
            mDeformationTexture[mCurrentTextureIndex].get());

        // Update shader uniforms
        osg::Uniform* currentTimeUniform = mDecayStateSet->getUniform("currentTime");
        if (currentTimeUniform)
            currentTimeUniform->set(mCurrentTime);

        // Enable decay rendering for this frame
        mDecayGroup->setNodeMask(~0u);
        mRTTCamera->setNodeMask(~0u);

        // Other groups already disabled at start of update()

        static int logCount = 0;
        if (logCount++ < 5)
        {
            Log(Debug::Info) << "[SNOW] Applying decay at time " << mCurrentTime;
        }
    }

    void SnowDeformationManager::updateTerrainParameters(const osg::Vec3f& playerPos)
    {
        // Detect terrain texture at player position
        std::string terrainType = detectTerrainTexture(playerPos);

        // Only update if terrain type changed
        if (terrainType == mCurrentTerrainType)
            return;

        mCurrentTerrainType = terrainType;

        // Find matching parameters
        for (const auto& params : mTerrainParams)
        {
            if (terrainType.find(params.pattern) != std::string::npos)
            {
                mFootprintRadius = params.radius;
                mDeformationDepth = params.depth;
                mFootprintInterval = params.interval;

                Log(Debug::Info) << "[SNOW] Terrain type changed to '" << terrainType
                                << "' - radius=" << params.radius
                                << ", depth=" << params.depth
                                << ", interval=" << params.interval;
                return;
            }
        }

        // Default to snow parameters if no match
        Log(Debug::Info) << "[SNOW] Unknown terrain type '" << terrainType << "', using snow defaults";
    }

    std::string SnowDeformationManager::detectTerrainTexture(const osg::Vec3f& worldPos)
    {
        // TODO: Implement actual terrain texture detection by querying terrain storage
        // For now, return snow for testing
        // Real implementation would:
        // 1. Get terrain chunk at worldPos
        // 2. Query texture layers for that chunk
        // 3. Sample blendmaps at player's UV coordinate
        // 4. Return the dominant texture name

        // Placeholder: always return "snow"
        return "snow";
    }

    void SnowDeformationManager::getDeformationParams(float& outRadius, float& outDepth, float& outInterval) const
    {
        outRadius = mFootprintRadius;
        outDepth = mDeformationDepth;
        outInterval = mFootprintInterval;
    }
}
