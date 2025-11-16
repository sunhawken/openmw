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
    // ============================================================================
    // TRAIL SYSTEM CONFIGURATION
    // ============================================================================
    // Default trail decay time: 3 minutes (180 seconds)
    // - Trails remain visible for several minutes before fading
    // - Configurable via mDecayTime
    // ============================================================================
    static constexpr float DEFAULT_TRAIL_DECAY_TIME = 180.0f;  // 3 minutes

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
        , mDecayTime(DEFAULT_TRAIL_DECAY_TIME)  // TRAIL DECAY: Trails fade out over 3 minutes
        , mTimeSinceLastDecay(0.0f)
        , mDecayUpdateInterval(0.1f)  // Apply decay every 0.1 seconds for smooth restoration
        , mCurrentTerrainType("snow")
        , mCurrentTime(0.0f)
    {
        Log(Debug::Info) << "[SNOW] SnowDeformationManager created with trail decay time: " << mDecayTime << "s";

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
            // ====================================================================
            // SNOW TRAIL SYSTEM - Footprint Stamping Shader (Inline Version)
            // ====================================================================
            // Implements non-additive trail system with age preservation
            // Matches external shader: files/shaders/compatibility/snow_footprint.frag
            // ====================================================================

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
                // Sample previous deformation state
                vec4 prevDeform = texture2D(previousDeformation, texUV);
                float prevDepth = prevDeform.r;  // Previous deformation depth
                float prevAge = prevDeform.g;    // Timestamp when first deformed

                // Convert UV (0-1) to world position
                // UV (0,0) = bottom-left, UV (1,1) = top-right
                vec2 worldPos = deformationCenter + (texUV - 0.5) * 2.0 * deformationRadius;

                // Calculate distance from current footprint center
                float dist = length(worldPos - footprintCenter);

                // Smooth circular falloff for realistic footprint shape
                float influence = 1.0 - smoothstep(footprintRadius * 0.5, footprintRadius, dist);

                // Calculate new footprint depth
                float newFootprintDepth = influence * deformationDepth;

                // NON-ADDITIVE: Use max() blending so multiple passes don't deepen snow
                float newDepth = max(prevDepth, newFootprintDepth);

                // AGE PRESERVATION: Don't reset age on repeat passes
                // This creates "plowing through snow" effect where trails don't refresh
                float age;
                if (prevDepth > 0.01)
                {
                    // Already deformed - preserve original age (no refresh)
                    age = prevAge;
                }
                else if (newFootprintDepth > 0.01)
                {
                    // Fresh snow being deformed - mark with current time
                    age = currentTime;
                }
                else
                {
                    // No deformation - keep previous age (if any)
                    age = prevAge;
                }

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

        // ====================================================================
        // TRAIL SYSTEM UPDATE PRIORITY
        // ====================================================================
        // Only ONE RTT operation per frame to avoid ping-pong conflicts
        // Priority order: blit > footprint > decay
        //
        // - Blit: Highest priority - must preserve trails when recentering
        // - Footprint: Medium priority - create new trail deformation
        // - Decay: Lowest priority - gradual restoration can wait
        // ====================================================================

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

        // ====================================================================
        // TRAIL CREATION
        // ====================================================================
        // Stamps a footprint at the player's current position
        //
        // Non-additive behavior:
        // - Shader uses max() blending (doesn't deepen existing trails)
        // - Age is preserved on repeat passes (doesn't refresh decay timer)
        // - Creates "plowing through snow" effect
        // ====================================================================

        Log(Debug::Info) << "[SNOW TRAIL] Stamping footprint at "
                        << (int)position.x() << ", " << (int)position.y()
                        << " with depth=" << mDeformationDepth
                        << ", radius=" << mFootprintRadius
                        << ", time=" << mCurrentTime;

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

        // AUTO-SAVE: Save first few footprints for verification
        if (stampCount == 5 || stampCount == 10 || stampCount == 20)
        {
            std::string filename = "snow_footprint_" + std::to_string(stampCount) + ".png";
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Auto-saving texture after " << stampCount << " footprints";
            // Note: Saving is deferred to next frame via callback
            saveDeformationTexture(filename, true);
        }
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
            // ====================================================================
            // SNOW TRAIL SYSTEM - Decay Shader
            // ====================================================================
            // Gradually restores snow surface to pristine state over time
            //
            // Decay Behavior:
            // - Linear decay based on time since first deformation
            // - Trails fade out smoothly over configured decay time
            // - Age is preserved (doesn't reset) for consistent decay
            // ====================================================================

            uniform sampler2D currentDeformation;  // Current deformation texture
            uniform float currentTime;             // Current game time
            uniform float decayTime;               // Time for complete restoration (e.g., 180s = 3 minutes)
            varying vec2 texUV;

            void main()
            {
                // Sample current deformation state
                vec4 deform = texture2D(currentDeformation, texUV);
                float depth = deform.r;  // Current deformation depth
                float age = deform.g;    // Timestamp when first deformed

                // Only apply decay if there's deformation present
                if (depth > 0.01)
                {
                    // Calculate how long ago this area was first deformed
                    float timeSinceCreation = currentTime - age;

                    // Linear decay factor (0.0 = fresh, 1.0 = fully decayed)
                    // Trail fades out gradually over decayTime seconds
                    float decayFactor = clamp(timeSinceCreation / decayTime, 0.0, 1.0);

                    // Reduce depth by decay factor
                    // depth * (1.0 - decayFactor):
                    //   - At t=0:          decayFactor=0.0, depth unchanged (100%)
                    //   - At t=decayTime:  decayFactor=1.0, depth=0.0 (fully restored)
                    depth *= (1.0 - decayFactor);

                    // Clean up very small depths to avoid floating point artifacts
                    if (depth < 0.01)
                        depth = 0.0;
                }

                // Output updated deformation
                // Age is preserved - decay continues based on original timestamp
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

        // ====================================================================
        // TRAIL DECAY SYSTEM
        // ====================================================================
        // Gradually restores snow surface over time
        //
        // Behavior:
        // - Linear decay based on time since first deformation
        // - Age is preserved (decay continues from original timestamp)
        // - Trails fade out smoothly over mDecayTime seconds (default 180s)
        // ====================================================================

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

    void SnowDeformationManager::saveDeformationTexture(const std::string& filename, bool debugInfo)
    {
        // ====================================================================
        // DIAGNOSTIC: Save RTT deformation texture for inspection
        // ====================================================================
        // This function captures the current deformation texture from GPU
        // and saves it to disk for debugging purposes
        // ====================================================================

        if (!mActive || !mDeformationTexture[mCurrentTextureIndex])
        {
            Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Cannot save texture - system not active or texture invalid";
            return;
        }

        osg::Texture2D* tex = mDeformationTexture[mCurrentTextureIndex].get();

        // Get or create Image from texture
        osg::ref_ptr<osg::Image> image = tex->getImage();
        if (!image)
        {
            // RTT textures don't have CPU-side images by default
            // Create one and request GPU readback
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Creating image for GPU readback...";

            image = new osg::Image;
            image->allocateImage(mTextureResolution, mTextureResolution, 1, GL_RGBA, GL_FLOAT);

            // Attach a camera callback to read back the texture data
            // This is necessary because RTT textures are GPU-only
            struct ReadbackCallback : public osg::Camera::DrawCallback
            {
                osg::ref_ptr<osg::Image> targetImage;
                osg::ref_ptr<osg::Texture2D> sourceTexture;
                std::string saveFilename;
                bool includeDebugInfo;
                osg::Vec2f textureCenter;
                float textureRadius;
                osg::Vec3f playerPos;

                ReadbackCallback(osg::Image* img, osg::Texture2D* tex, const std::string& filename,
                                bool debugInfo, const osg::Vec2f& center, float radius, const osg::Vec3f& player)
                    : targetImage(img), sourceTexture(tex), saveFilename(filename)
                    , includeDebugInfo(debugInfo), textureCenter(center)
                    , textureRadius(radius), playerPos(player)
                {}

                virtual void operator()(osg::RenderInfo& renderInfo) const
                {
                    // Read pixels from framebuffer
                    glReadPixels(0, 0, targetImage->s(), targetImage->t(),
                                GL_RGBA, GL_FLOAT, targetImage->data());

                    Log(Debug::Info) << "[SNOW DIAGNOSTIC] Texture readback complete, saving to " << saveFilename;

                    // Convert RGBA16F to RGBA8 for saving
                    osg::ref_ptr<osg::Image> saveImage = new osg::Image;
                    saveImage->allocateImage(targetImage->s(), targetImage->t(), 1, GL_RGBA, GL_UNSIGNED_BYTE);

                    const float* srcData = (const float*)targetImage->data();
                    unsigned char* dstData = saveImage->data();

                    for (int i = 0; i < targetImage->s() * targetImage->t(); ++i)
                    {
                        // R channel = depth (0.0-1.0)
                        // G channel = age (game time, potentially very large)
                        // Visualize depth in R channel, normalize age for G channel

                        float depth = srcData[i * 4 + 0];
                        float age = srcData[i * 4 + 1];

                        // Depth → Red channel (direct mapping)
                        dstData[i * 4 + 0] = static_cast<unsigned char>(depth * 255.0f);

                        // Age → Green channel (show if deformed)
                        dstData[i * 4 + 1] = (depth > 0.01f) ? 255 : 0;

                        // Blue → unused
                        dstData[i * 4 + 2] = 0;

                        // Alpha
                        dstData[i * 4 + 3] = 255;
                    }

                    // Save to file
                    if (osgDB::writeImageFile(*saveImage, saveFilename))
                    {
                        Log(Debug::Info) << "[SNOW DIAGNOSTIC] Texture saved successfully!";

                        if (includeDebugInfo)
                        {
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] ====== DEBUG INFO ======";
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Player position: ("
                                           << (int)playerPos.x() << ", "
                                           << (int)playerPos.y() << ", "
                                           << (int)playerPos.z() << ")";
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Texture center: ("
                                           << (int)textureCenter.x() << ", "
                                           << (int)textureCenter.y() << ")";
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Texture radius: " << textureRadius;
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Camera eye: ("
                                           << (int)playerPos.x() << ", "
                                           << (int)playerPos.y() << ", "
                                           << (int)(playerPos.z() + 100.0f) << ")";
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Camera look-at: ("
                                           << (int)playerPos.x() << ", "
                                           << (int)playerPos.y() << ", "
                                           << (int)playerPos.z() << ")";
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Camera up: (0, -1, 0) [-Y = South]";
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Coordinate system: X=East/West, Y=North/South, Z=Up";
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Visualization: Red=Depth, Green=Deformed areas";
                        }
                    }
                    else
                    {
                        Log(Debug::Error) << "[SNOW DIAGNOSTIC] Failed to save texture to " << saveFilename;
                    }
                }
            };

            // Attach callback to next frame
            osg::ref_ptr<ReadbackCallback> callback = new ReadbackCallback(
                image.get(), tex, filename, debugInfo,
                mTextureCenter, mWorldTextureRadius, mCurrentPlayerPos
            );

            mRTTCamera->setFinalDrawCallback(callback);

            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Readback callback attached, will save on next frame";
        }
        else
        {
            Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Texture already has an Image - this shouldn't happen for RTT textures";
        }
    }
}
