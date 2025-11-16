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
#include <osg/FrameBufferObject>
#include <osg/GLExtensions>
#include <osg/Viewport>
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
        // ====================================================================
        // RTT CAMERA SETUP - REWRITTEN FOR CORRECTNESS
        // ====================================================================
        // Key principles:
        // 1. Camera uses ABSOLUTE_RF (own coordinate system)
        // 2. Camera-local quads always at Z=0
        // 3. View matrix positions camera in world (updated per-frame)
        // 4. Orthographic projection with SMALL near/far (-10 to +10)
        // 5. Simple top-down view looking down -Z axis
        // ====================================================================

        mRTTCamera = new osg::Camera;

        // FBO render target
        mRTTCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mRTTCamera->setRenderOrder(osg::Camera::PRE_RENDER);

        // ABSOLUTE reference frame - camera has its own coordinate system
        // All child geometry is in camera-local space
        mRTTCamera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);

        // ORTHOGRAPHIC PROJECTION - Top-down view
        // Left/Right/Bottom/Top: Cover the world texture radius
        // Near/Far: Small range around Z=0 (camera-local space)
        //
        // CRITICAL: Quads will be at Z=0 in camera space
        // Near=-10, Far=+10 gives 20 units of depth buffer
        // This is sufficient since all quads are flat at Z=0
        mRTTCamera->setProjectionMatrixAsOrtho(
            -mWorldTextureRadius, mWorldTextureRadius,  // Left, Right (X range)
            -mWorldTextureRadius, mWorldTextureRadius,  // Bottom, Top (Y range)
            -10.0f, 10.0f                               // Near, Far (Z range in camera space)
        );

        // INITIAL VIEW MATRIX - Will be updated each frame via updateCameraPosition()
        // This is just a placeholder, actual position set when player position known
        //
        // Camera looks down negative Z axis (in camera space)
        // Eye: At origin, slightly above (+Z)
        // Center: At origin
        // Up: -Y direction (so +Y/North appears at top of texture)
        mRTTCamera->setViewMatrixAsLookAt(
            osg::Vec3(0.0f, 0.0f, 10.0f),   // Eye position (10 units up in camera space)
            osg::Vec3(0.0f, 0.0f, 0.0f),    // Look at center
            osg::Vec3(0.0f, -1.0f, 0.0f)    // Up = -Y (South), so North at top
        );

        // ====================================================================
        // CRITICAL TEST: DISABLE CLEAR TO SEE IF QUAD IS RENDERING
        // ====================================================================
        // If clear is overwriting geometry, disabling it should show accumulation
        // Temporarily set clear mask to 0 to test if anything renders at all
        // ====================================================================
        // ORIGINAL (disabled for testing):
        // mRTTCamera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // mRTTCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));

        // TEST: Disable clear completely
        mRTTCamera->setClearMask(0);  // No clearing - should accumulate if rendering
        mRTTCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));

        Log(Debug::Warning) << "[SNOW RTT TEST] *** CLEAR DISABLED FOR TESTING ***";
        Log(Debug::Warning) << "[SNOW RTT TEST] If quad renders, red should accumulate over frames";

        // Set viewport to match texture resolution
        // CRITICAL: Create viewport object that we can protect
        osg::ref_ptr<osg::Viewport> rttViewport = new osg::Viewport(0, 0, mTextureResolution, mTextureResolution);
        mRTTCamera->setViewport(rttViewport);

        // ====================================================================
        // FIX 1: OVERRIDE INHERITED STATE + PROTECT VIEWPORT
        // ====================================================================
        // The camera might be inheriting render state from the parent scene
        // that's preventing rendering. Explicitly override all state with
        // PROTECTED flags to ensure our settings take precedence.
        //
        // CRITICAL: Also add the viewport to the state set with PROTECTED flag
        // to prevent it from being changed during rendering!
        // ====================================================================
        osg::ref_ptr<osg::StateSet> cameraState = new osg::StateSet;
        cameraState->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
        cameraState->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
        cameraState->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
        cameraState->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);

        // CRITICAL FIX: Protect the viewport from being changed!
        // This prevents OSG or other code from changing the viewport during rendering
        cameraState->setAttributeAndModes(rttViewport, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);

        mRTTCamera->setStateSet(cameraState);

        Log(Debug::Warning) << "[SNOW RTT TEST] Viewport PROTECTED in camera state set to prevent changes during render";

        // ====================================================================
        // FIX 2: ADD DIAGNOSTIC CALLBACKS WITH ENHANCED LOGGING
        // ====================================================================
        // Add callbacks to verify the camera is actually executing
        // These will tell us WHEN and IF the camera is rendering
        // ====================================================================
        struct DiagnosticInitialDrawCallback : public osg::Camera::DrawCallback
        {
            mutable int callCount = 0;

            virtual void operator()(osg::RenderInfo& renderInfo) const override
            {
                callCount++;
                if (callCount <= 10)
                {
                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] ========================================";
                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] INITIAL Draw Callback #" << callCount << " - BEFORE RENDER";
                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] ========================================";

                    // Check FBO completeness using OSG's extension mechanism
                    osg::State* state = renderInfo.getState();
                    if (state)
                    {
                        const osg::GLExtensions* ext = state->get<osg::GLExtensions>();
                        if (ext && ext->isFrameBufferObjectSupported)
                        {
                            GLenum status = ext->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
                            const char* statusStr = "UNKNOWN";

                            if (status == GL_FRAMEBUFFER_COMPLETE_EXT)
                                statusStr = "COMPLETE";
                            else if (status == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_EXT)
                                statusStr = "INCOMPLETE_ATTACHMENT";
                            else if (status == GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_EXT)
                                statusStr = "MISSING_ATTACHMENT";
                            else if (status == GL_FRAMEBUFFER_UNSUPPORTED_EXT)
                                statusStr = "UNSUPPORTED";
                            else if (status == GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER_EXT)
                                statusStr = "INCOMPLETE_DRAW_BUFFER";
                            else if (status == GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER_EXT)
                                statusStr = "INCOMPLETE_READ_BUFFER";

                            Log(Debug::Warning) << "[SNOW DIAGNOSTIC] FBO status: " << status << " (" << statusStr << ")";

                            if (status != GL_FRAMEBUFFER_COMPLETE_EXT)
                            {
                                Log(Debug::Error) << "[SNOW DIAGNOSTIC] *** FBO NOT COMPLETE! ***";
                                Log(Debug::Error) << "[SNOW DIAGNOSTIC] This means the framebuffer is not ready to render!";
                            }
                            else
                            {
                                Log(Debug::Warning) << "[SNOW DIAGNOSTIC] FBO is COMPLETE - ready to render";
                            }
                        }
                        else
                        {
                            Log(Debug::Warning) << "[SNOW DIAGNOSTIC] FBO extensions not available";
                        }

                        // Get current viewport from GL state
                        GLint glViewport[4];
                        glGetIntegerv(GL_VIEWPORT, glViewport);
                        Log(Debug::Warning) << "[SNOW DIAGNOSTIC] GL Viewport: " << glViewport[0] << "," << glViewport[1]
                                           << " " << glViewport[2] << "x" << glViewport[3];

                        // Check what OSG thinks the viewport should be
                        // The camera should have been told to use 1024x1024
                        osg::Camera* cam = dynamic_cast<osg::Camera*>(renderInfo.getCurrentCamera());
                        if (cam && cam->getViewport())
                        {
                            const osg::Viewport* vp = cam->getViewport();
                            Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Camera's Viewport Setting: "
                                               << vp->x() << "," << vp->y() << " "
                                               << vp->width() << "x" << vp->height();

                            // CRITICAL FIX: Force the viewport to match camera's setting
                            if (glViewport[2] != (GLint)vp->width() || glViewport[3] != (GLint)vp->height())
                            {
                                Log(Debug::Error) << "[SNOW DIAGNOSTIC] *** VIEWPORT MISMATCH! ***";
                                Log(Debug::Error) << "[SNOW DIAGNOSTIC] GL viewport doesn't match camera viewport!";
                                Log(Debug::Error) << "[SNOW DIAGNOSTIC] Expected: " << vp->width() << "x" << vp->height();
                                Log(Debug::Error) << "[SNOW DIAGNOSTIC] Got: " << glViewport[2] << "x" << glViewport[3];
                                Log(Debug::Error) << "[SNOW DIAGNOSTIC] Forcing viewport via OSG State...";

                                // Use OSG's state mechanism to apply viewport
                                // This is the portable way to set viewport in OSG
                                state->applyAttribute(vp);

                                // Verify it stuck
                                GLint newViewport[4];
                                glGetIntegerv(GL_VIEWPORT, newViewport);
                                Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Viewport after force: "
                                                   << newViewport[0] << "," << newViewport[1]
                                                   << " " << newViewport[2] << "x" << newViewport[3];

                                if (newViewport[2] == (GLint)vp->width() && newViewport[3] == (GLint)vp->height())
                                {
                                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] ✓ Viewport correction SUCCEEDED!";
                                }
                                else
                                {
                                    Log(Debug::Error) << "[SNOW DIAGNOSTIC] ✗ Viewport correction FAILED - still wrong!";
                                }
                            }
                        }
                        else
                        {
                            Log(Debug::Error) << "[SNOW DIAGNOSTIC] *** Camera has NO viewport set! ***";
                        }
                    }
                    else
                    {
                        Log(Debug::Warning) << "[SNOW DIAGNOSTIC] No GL state available in initial callback";
                    }
                }
            }
        };

        struct DiagnosticFinalDrawCallback : public osg::Camera::DrawCallback
        {
            mutable int callCount = 0;

            virtual void operator()(osg::RenderInfo& renderInfo) const override
            {
                callCount++;
                if (callCount <= 10)
                {
                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] ========================================";
                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] FINAL Draw Callback #" << callCount << " - AFTER RENDER";
                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] ========================================";
                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] If you see this, the camera DID execute rendering";

                    // Verify viewport stayed correct during rendering
                    GLint viewport[4];
                    glGetIntegerv(GL_VIEWPORT, viewport);
                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Final viewport: " << viewport[0] << "," << viewport[1]
                                       << " " << viewport[2] << "x" << viewport[3];
                }
            }
        };

        struct DiagnosticCullCallback : public osg::NodeCallback
        {
            mutable int callCount = 0;

            virtual void operator()(osg::Node* node, osg::NodeVisitor* nv) override
            {
                callCount++;
                if (callCount <= 10)
                {
                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] ========================================";
                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Cull Callback #" << callCount;
                    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] ========================================";
                    osg::Camera* cam = dynamic_cast<osg::Camera*>(node);
                    if (cam)
                    {
                        Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Camera node mask: " << cam->getNodeMask();
                        Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Camera children: " << cam->getNumChildren();
                        auto attachments = cam->getBufferAttachmentMap();
                        Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Buffer attachments: " << attachments.size();
                    }
                }
                traverse(node, nv);
            }
        };

        mRTTCamera->setInitialDrawCallback(new DiagnosticInitialDrawCallback);
        mRTTCamera->setFinalDrawCallback(new DiagnosticFinalDrawCallback);
        mRTTCamera->setCullCallback(new DiagnosticCullCallback);

        // Start disabled (enabled when stamping footprints)
        mRTTCamera->setNodeMask(0);

        // Add to scene graph
        rootNode->addChild(mRTTCamera);

        Log(Debug::Warning) << "[SNOW RTT TEST] ========================================";
        Log(Debug::Warning) << "[SNOW RTT TEST] Camera created with TEST CONFIGURATION:";
        Log(Debug::Warning) << "[SNOW RTT TEST] ========================================";
        Log(Debug::Warning) << "[SNOW RTT TEST] Resolution: " << mTextureResolution << "x" << mTextureResolution;
        Log(Debug::Warning) << "[SNOW RTT TEST] Projection: Ortho(" << -mWorldTextureRadius << " to " << mWorldTextureRadius << ")";
        Log(Debug::Warning) << "[SNOW RTT TEST] Near/Far: -10 to +10 (camera-local)";
        Log(Debug::Warning) << "[SNOW RTT TEST] Clear: *** DISABLED FOR TESTING ***";
        Log(Debug::Warning) << "[SNOW RTT TEST] Texture format: GL_RGBA8 (was GL_RGBA16F)";
        Log(Debug::Warning) << "[SNOW RTT TEST] Reference frame: ABSOLUTE_RF";
        Log(Debug::Warning) << "[SNOW RTT TEST] State overrides: LIGHTING/DEPTH/CULL all OFF+PROTECTED";
        Log(Debug::Warning) << "[SNOW RTT TEST] Diagnostic callbacks: INITIAL + FINAL + CULL";
        Log(Debug::Warning) << "[SNOW RTT TEST] ========================================";
        Log(Debug::Warning) << "[SNOW RTT TEST] Expected: If quad renders, callbacks will show FBO COMPLETE";
        Log(Debug::Warning) << "[SNOW RTT TEST] Expected: Red pixels should accumulate (clear disabled)";
        Log(Debug::Warning) << "[SNOW RTT TEST] Expected: Max depth byte should be 255 (from vec4(1.0,0,0,1))";
        Log(Debug::Warning) << "[SNOW RTT TEST] ========================================";
    }

    void SnowDeformationManager::createDeformationTextures()
    {
        // Create ping-pong textures for accumulation
        // ====================================================================
        // TEST: Using GL_RGBA8 instead of GL_RGBA16F
        // ====================================================================
        // Float formats might have driver issues. Test with unsigned byte first.
        // ====================================================================
        for (int i = 0; i < 2; ++i)
        {
            mDeformationTexture[i] = new osg::Texture2D;
            mDeformationTexture[i]->setTextureSize(mTextureResolution, mTextureResolution);
            // TEST: Changed from GL_RGBA16F_ARB to GL_RGBA8
            mDeformationTexture[i]->setInternalFormat(GL_RGBA8);
            mDeformationTexture[i]->setSourceFormat(GL_RGBA);
            // TEST: Changed from GL_FLOAT to GL_UNSIGNED_BYTE
            mDeformationTexture[i]->setSourceType(GL_UNSIGNED_BYTE);
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

            Log(Debug::Warning) << "[SNOW RTT TEST] Created deformation texture " << i
                            << " (" << mTextureResolution << "x" << mTextureResolution << ")"
                            << " Format: GL_RGBA8 (TESTING)";
        }

        mCurrentTextureIndex = 0;

        // Attach first texture to RTT camera
        mRTTCamera->attach(osg::Camera::COLOR_BUFFER, mDeformationTexture[0].get());

        // ====================================================================
        // FIX 3: ADD EXPLICIT DEPTH BUFFER
        // ====================================================================
        // Some OSG implementations require an explicit depth attachment
        // for the FBO to be considered complete, even if we don't use depth.
        // ====================================================================
        osg::ref_ptr<osg::Texture2D> depthTexture = new osg::Texture2D;
        depthTexture->setTextureSize(mTextureResolution, mTextureResolution);
        depthTexture->setInternalFormat(GL_DEPTH_COMPONENT);
        depthTexture->setSourceFormat(GL_DEPTH_COMPONENT);
        depthTexture->setSourceType(GL_FLOAT);
        mRTTCamera->attach(osg::Camera::DEPTH_BUFFER, depthTexture.get());

        Log(Debug::Warning) << "[SNOW RTT TEST] Deformation textures created (ping-pong) with depth buffer";
        Log(Debug::Warning) << "[SNOW RTT TEST] Depth buffer: GL_DEPTH_COMPONENT, " << mTextureResolution << "x" << mTextureResolution;
    }

    void SnowDeformationManager::setupFootprintStamping()
    {
        // ====================================================================
        // FOOTPRINT STAMPING SETUP - REWRITTEN
        // ====================================================================
        // Key changes:
        // 1. Quad ALWAYS at Z=0 in camera-local space
        // 2. Never modified after creation
        // 3. Camera view matrix handles world positioning
        // ====================================================================

        // Create group for footprint geometry
        mFootprintGroup = new osg::Group;
        mRTTCamera->addChild(mFootprintGroup);

        // Create full-screen quad for footprint stamping
        mFootprintQuad = new osg::Geometry;
        mFootprintQuad->setUseDisplayList(false);
        mFootprintQuad->setUseVertexBufferObjects(true);

        // QUAD VERTICES - Camera-local space
        // TESTING DIFFERENT Z POSITIONS
        //
        // The camera view matrix positions the camera in world space, but the quad
        // is defined in camera-local space. We need to find the correct Z position
        // that makes the quad visible to the orthographic camera.
        //
        // Orthographic projection: near=-10, far=+10
        // Let's try Z=0 (at the camera's origin in camera space)
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3(-mWorldTextureRadius, -mWorldTextureRadius, 0.0f));  // Bottom-left
        vertices->push_back(osg::Vec3( mWorldTextureRadius, -mWorldTextureRadius, 0.0f));  // Bottom-right
        vertices->push_back(osg::Vec3( mWorldTextureRadius,  mWorldTextureRadius, 0.0f));  // Top-right
        vertices->push_back(osg::Vec3(-mWorldTextureRadius,  mWorldTextureRadius, 0.0f));  // Top-left
        mFootprintQuad->setVertexArray(vertices);

        // UV coordinates (0-1 mapping to texture)
        osg::ref_ptr<osg::Vec2Array> uvs = new osg::Vec2Array;
        uvs->push_back(osg::Vec2(0.0f, 0.0f));  // Bottom-left
        uvs->push_back(osg::Vec2(1.0f, 0.0f));  // Bottom-right
        uvs->push_back(osg::Vec2(1.0f, 1.0f));  // Top-right
        uvs->push_back(osg::Vec2(0.0f, 1.0f));  // Top-left
        mFootprintQuad->setTexCoordArray(0, uvs);

        // Normals (facing camera - negative Z in camera space)
        // The camera looks down -Z axis, so quad should face back toward +Z to be visible
        osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
        normals->push_back(osg::Vec3(0.0f, 0.0f, 1.0f));  // Face toward +Z (toward camera eye)
        mFootprintQuad->setNormalArray(normals);
        mFootprintQuad->setNormalBinding(osg::Geometry::BIND_OVERALL);

        // Quad primitive (4 vertices forming a rectangle)
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
            // SNOW TRAIL SYSTEM - Footprint Stamping Shader (TEST VERSION)
            // ====================================================================
            // TEMPORARY: Simplified shader to test if rendering works at all
            // Just outputs solid red to verify the quad is visible
            // ====================================================================

            varying vec2 texUV;

            void main()
            {
                // TEST: Just output solid red (max depth = 1.0)
                // If this works, we'll see Max depth = 1.0 in logs
                // If still 0, the quad isn't rendering at all
                gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
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

        // Disable depth testing - we're rendering to a 2D texture
        mFootprintStateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);

        // Disable backface culling - ensure quad is always visible regardless of orientation
        mFootprintStateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);

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

        // CRITICAL TIMING FIX:
        // Do NOT disable groups here at the start of update()!
        //
        // OSG Frame Flow:
        //   Frame N: Update (enable camera) → Draw (PRE_RENDER executes)
        //   Frame N+1: Update → Draw
        //
        // If we disable at START of Frame N+1 update, the camera is disabled
        // before Frame N's draw phase completes, so it never renders!
        //
        // Solution: Disable/enable happens within stampFootprint/blit/decay.
        // Each function explicitly disables the OTHER groups and enables its own.
        // This ensures only one group is active at a time without premature disabling.

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
        // ====================================================================
        // UPDATE CAMERA POSITION - REWRITTEN
        // ====================================================================
        // The camera view matrix transforms from world space to camera space
        // Camera positioned above player, looking straight down
        //
        // World coordinate system (OpenMW):
        //   X = East/West
        //   Y = North/South
        //   Z = Up/Down (altitude)
        //
        // Camera positioning:
        //   Eye: Player XY position, 100 units above player altitude
        //   Center: Player XY position, AT player altitude
        //   Up: -Y (South) so +Y (North) appears at top of texture
        // ====================================================================

        // Update texture center (2D ground plane position)
        mTextureCenter.set(playerPos.x(), playerPos.y());

        // Position camera in world space via view matrix
        // The camera is positioned 100 units directly above the player
        // and looks straight down at the player's position
        if (mRTTCamera)
        {
            // Eye position: 100 units above player in world space
            osg::Vec3 eyePos(playerPos.x(), playerPos.y(), playerPos.z() + 100.0f);

            // Look-at point: Player's actual world position
            osg::Vec3 centerPos(playerPos.x(), playerPos.y(), playerPos.z());

            // Up vector: -Y (South) ensures North (+Y) is at top of texture
            osg::Vec3 upVector(0.0f, -1.0f, 0.0f);

            mRTTCamera->setViewMatrixAsLookAt(eyePos, centerPos, upVector);

            static int logCount = 0;
            if (logCount++ < 3)
            {
                Log(Debug::Info) << "[SNOW RTT REWRITE] Camera positioned:"
                                << " Eye=(" << (int)eyePos.x() << "," << (int)eyePos.y() << "," << (int)eyePos.z() << ")"
                                << " Center=(" << (int)centerPos.x() << "," << (int)centerPos.y() << "," << (int)centerPos.z() << ")"
                                << " | Player altitude=" << (int)playerPos.z()
                                << " | Texture center=(" << (int)mTextureCenter.x() << "," << (int)mTextureCenter.y() << ")";
            }
        }
    }

    void SnowDeformationManager::stampFootprint(const osg::Vec3f& position)
    {
        if (!mFootprintStateSet || !mRTTCamera || !mFootprintQuad)
            return;

        // ====================================================================
        // STAMP FOOTPRINT - REWRITTEN
        // ====================================================================
        // Renders a footprint at the player's current world position
        //
        // How it works:
        // 1. Swap ping-pong textures (previous becomes input, current becomes output)
        // 2. Update shader uniforms with footprint world position
        // 3. Enable RTT camera to render the footprint quad
        // 4. Fragment shader samples previous texture and adds new footprint
        //
        // Key: The quad stays at Z=0 in camera space
        //      The camera view matrix positions it correctly in world space
        // ====================================================================

        Log(Debug::Info) << "[SNOW TRAIL] Stamping footprint at world pos ("
                        << (int)position.x() << ", " << (int)position.y() << ", " << (int)position.z() << ")"
                        << " | depth=" << mDeformationDepth
                        << " | radius=" << mFootprintRadius
                        << " | time=" << mCurrentTime;

        // NO QUAD MODIFICATION NEEDED!
        // The quad is always at Z=0 in camera-local space
        // The camera's view matrix (set by updateCameraPosition) handles world positioning
        //
        // Camera transform chain:
        //   World space → View matrix → Camera space → Projection → NDC
        //
        // Since camera uses ABSOLUTE_RF, the view matrix fully controls world positioning
        // The quad at camera-local Z=0 will be correctly positioned in world space

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
        // CRITICAL: The camera must be enabled AND we need to ensure it actually renders
        mRTTCamera->setNodeMask(~0u);
        mFootprintGroup->setNodeMask(~0u);

        // Disable other groups to ensure only footprint renders
        if (mBlitGroup)
            mBlitGroup->setNodeMask(0);
        if (mDecayGroup)
            mDecayGroup->setNodeMask(0);

        // IMPORTANT: Don't set a disable callback here
        // The camera will be disabled on the next frame OR by the save callback
        // For now, just let it stay enabled and we'll disable it next update()

        // DIAGNOSTIC: Log detailed state
        static int stampCount = 0;
        stampCount++;

        // DIAGNOSTIC: Check camera setup (moved here after stampCount declaration)
        if (stampCount <= 3)
        {
            osg::Matrixd proj = mRTTCamera->getProjectionMatrix();
            osg::Matrixd view = mRTTCamera->getViewMatrix();
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Projection matrix valid: " << proj.valid();
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] View matrix valid: " << view.valid();
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Camera reference frame: " << mRTTCamera->getReferenceFrame();
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Camera render order: " << mRTTCamera->getRenderOrder();
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Camera clear mask: " << mRTTCamera->getClearMask();
        }

        if (stampCount <= 3)
        {
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] === Footprint Stamp #" << stampCount << " ===";
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] RTT Camera node mask: " << mRTTCamera->getNodeMask();
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Footprint group node mask: " << mFootprintGroup->getNodeMask();
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Footprint group children: " << mFootprintGroup->getNumChildren();
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] RTT camera children: " << mRTTCamera->getNumChildren();
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Current texture attached: " << (mRTTCamera->getBufferAttachmentMap().count(osg::Camera::COLOR_BUFFER) > 0);
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Footprint center (world): (" << (int)position.x() << ", " << (int)position.y() << ")";
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Texture center (world): (" << (int)mTextureCenter.x() << ", " << (int)mTextureCenter.y() << ")";
            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Camera clear color: " << mRTTCamera->getClearColor().r() << ","
                            << mRTTCamera->getClearColor().g() << "," << mRTTCamera->getClearColor().b();
        }

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
        // ====================================================================
        // BLIT SYSTEM SETUP - REWRITTEN
        // ====================================================================
        // Blit copies the old texture to a new position when the camera moves
        // Same principle: quad at Z=0 in camera space, never modified
        // ====================================================================

        mBlitGroup = new osg::Group;
        mRTTCamera->addChild(mBlitGroup);

        mBlitQuad = new osg::Geometry;
        mBlitQuad->setUseDisplayList(false);
        mBlitQuad->setUseVertexBufferObjects(true);

        // Full-screen quad at Z=0 in camera-local space
        // NEVER modified after creation
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
        // ====================================================================
        // DECAY SYSTEM SETUP - REWRITTEN
        // ====================================================================
        // Decay gradually restores snow over time
        // Same principle: quad at Z=0 in camera space, never modified
        // ====================================================================

        mDecayGroup = new osg::Group;
        mRTTCamera->addChild(mDecayGroup);

        mDecayQuad = new osg::Geometry;
        mDecayQuad->setUseDisplayList(false);
        mDecayQuad->setUseVertexBufferObjects(true);

        // Full-screen quad at Z=0 in camera-local space
        // NEVER modified after creation
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

        // ====================================================================
        // BLIT TEXTURE - REWRITTEN
        // ====================================================================
        // Copies old texture content to new position when camera recenters
        // No quad modification needed - camera view matrix handles positioning
        // ====================================================================

        Log(Debug::Info) << "[SNOW BLIT] Texture recenter: from ("
                        << (int)oldCenter.x() << ", " << (int)oldCenter.y() << ") to ("
                        << (int)newCenter.x() << ", " << (int)newCenter.y() << ")";

        // NO QUAD MODIFICATION NEEDED!
        // The blit quad stays at Z=0 in camera-local space
        // The camera's view matrix (already updated by updateCameraPosition) positions it correctly

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

        // Disable other groups to ensure only blit renders
        if (mFootprintGroup)
            mFootprintGroup->setNodeMask(0);
        if (mDecayGroup)
            mDecayGroup->setNodeMask(0);
    }

    void SnowDeformationManager::applyDecay(float dt)
    {
        if (!mDecayStateSet || !mRTTCamera || !mDecayQuad)
            return;

        // ====================================================================
        // APPLY DECAY - REWRITTEN
        // ====================================================================
        // Gradually restores snow surface over time
        //
        // Behavior:
        // - Linear decay based on time since first deformation
        // - Age is preserved (decay continues from original timestamp)
        // - Trails fade out smoothly over mDecayTime seconds (default 180s)
        //
        // No quad modification needed - camera view matrix handles positioning
        // ====================================================================

        // NO QUAD MODIFICATION NEEDED!
        // The decay quad stays at Z=0 in camera-local space
        // The camera's view matrix (already updated by updateCameraPosition) positions it correctly

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

        // Disable other groups to ensure only decay renders
        if (mFootprintGroup)
            mFootprintGroup->setNodeMask(0);
        if (mBlitGroup)
            mBlitGroup->setNodeMask(0);

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
        // SAVE DEFORMATION TEXTURE - REWRITTEN TO FIX CRASH
        // ====================================================================
        // Previous implementation crashed due to:
        // 1. glReadPixels called on wrong framebuffer
        // 2. DrawCallback timing issues
        // 3. No error checking
        //
        // New approach: Read from the existing texture attachment
        // Don't replace it - just read the current render target
        // ====================================================================

        if (!mActive || !mDeformationTexture[mCurrentTextureIndex])
        {
            Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Cannot save - system inactive or texture invalid";
            return;
        }

        // Get the current deformation texture that's attached to the camera
        // This is the texture the camera is actively rendering to
        osg::Texture2D* currentTexture = mDeformationTexture[mCurrentTextureIndex].get();

        Log(Debug::Info) << "[SNOW DIAGNOSTIC REWRITE] Preparing to save texture";
        Log(Debug::Info) << "[SNOW DIAGNOSTIC REWRITE] Current texture index: " << mCurrentTextureIndex;
        Log(Debug::Info) << "[SNOW DIAGNOSTIC REWRITE] Saving to file: " << filename;

        // Create a temporary image for readback
        // We'll use a camera callback to do the GPU->CPU transfer
        // IMPORTANT: Don't attach this as a render target - just read from existing texture
        struct TextureReadbackCallback : public osg::Camera::DrawCallback
        {
            osg::ref_ptr<osg::Texture2D> textureToRead;
            std::string filename;
            bool debugInfo;
            osg::Vec2f textureCenter;
            float textureRadius;
            osg::Vec3f playerPos;
            int resolution;
            mutable bool executed;  // Track if we've run once

            TextureReadbackCallback(osg::Texture2D* tex, const std::string& fname, bool debug,
                                   const osg::Vec2f& center, float radius, const osg::Vec3f& player, int res)
                : textureToRead(tex), filename(fname), debugInfo(debug)
                , textureCenter(center), textureRadius(radius), playerPos(player), resolution(res)
                , executed(false)
            {}

            virtual void operator()(osg::RenderInfo& renderInfo) const override
            {
                // Only execute once to avoid lag
                if (executed)
                    return;

                executed = true;

                Log(Debug::Info) << "[SNOW DIAGNOSTIC] Readback callback executing (one-time)";

                if (!textureToRead)
                {
                    Log(Debug::Error) << "[SNOW DIAGNOSTIC] Texture is null!";
                    return;
                }

                // Create a temporary image and bind the texture to read from it
                // This reads the GPU texture data to CPU memory
                // UPDATED: Now using GL_UNSIGNED_BYTE to match new texture format
                osg::ref_ptr<osg::Image> readImage = new osg::Image;
                readImage->allocateImage(resolution, resolution, 1, GL_RGBA, GL_UNSIGNED_BYTE);

                // Bind the texture and read its contents
                osg::State* state = renderInfo.getState();
                if (state)
                {
                    // Apply the texture (binds it to GL context)
                    textureToRead->apply(*state);

                    // Read the texture data from GPU (using GL_UNSIGNED_BYTE now)
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, readImage->data());

                    Log(Debug::Info) << "[SNOW DIAGNOSTIC] Texture data read from GPU";

                    // Already in RGBA8 format, can save directly
                    // But let's analyze the data first
                    const unsigned char* srcData = readImage->data();

                    int pixelCount = resolution * resolution;
                    int maxDepthByte = 0;
                    int nonZeroPixels = 0;

                    for (int i = 0; i < pixelCount; ++i)
                    {
                        int depth = srcData[i * 4 + 0];  // R channel = depth
                        int age = srcData[i * 4 + 1];    // G channel = age

                        if (depth > maxDepthByte)
                            maxDepthByte = depth;
                        if (depth > 0)
                            nonZeroPixels++;
                    }

                    float maxDepth = maxDepthByte / 255.0f;

                    Log(Debug::Info) << "[SNOW DIAGNOSTIC] Max depth byte: " << maxDepthByte << " (" << maxDepth << " normalized)";
                    Log(Debug::Info) << "[SNOW DIAGNOSTIC] Non-zero pixels: " << nonZeroPixels << " / " << pixelCount;

                    // Create visualization image (make depth more visible)
                    osg::ref_ptr<osg::Image> saveImage = new osg::Image;
                    saveImage->allocateImage(resolution, resolution, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                    unsigned char* dstData = saveImage->data();

                    for (int i = 0; i < pixelCount; ++i)
                    {
                        unsigned char depth = srcData[i * 4 + 0];

                        // Visualize: Red = depth value, Green = 255 if any depth present
                        dstData[i * 4 + 0] = depth;               // Red = actual depth
                        dstData[i * 4 + 1] = (depth > 0) ? 255 : 0;  // Green = has deformation
                        dstData[i * 4 + 2] = 0;                   // Blue = unused
                        dstData[i * 4 + 3] = 255;                 // Alpha = opaque
                    }

                    // Save to file
                    if (osgDB::writeImageFile(*saveImage, filename))
                    {
                        Log(Debug::Info) << "[SNOW DIAGNOSTIC] ✓ Texture saved: " << filename;
                        Log(Debug::Info) << "[SNOW DIAGNOSTIC] Max depth in texture: " << maxDepth;

                        if (debugInfo)
                        {
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] === Camera Debug Info ===";
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Player: (" << (int)playerPos.x()
                                           << ", " << (int)playerPos.y() << ", " << (int)playerPos.z() << ")";
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Texture center: ("
                                           << (int)textureCenter.x() << ", " << (int)textureCenter.y() << ")";
                            Log(Debug::Info) << "[SNOW DIAGNOSTIC] Texture radius: " << textureRadius;
                        }
                    }
                    else
                    {
                        Log(Debug::Error) << "[SNOW DIAGNOSTIC] ✗ Failed to save: " << filename;
                    }
                }
                else
                {
                    Log(Debug::Error) << "[SNOW DIAGNOSTIC] No GL state available!";
                }
            }
        };

        // IMPORTANT: Remove the callback after executing once to prevent lag
        // We need to clear it, but we can't do it from inside the callback
        // So we'll just let it run once and accept the small overhead

        // Attach the one-shot callback
        osg::ref_ptr<TextureReadbackCallback> callback = new TextureReadbackCallback(
            currentTexture, filename, debugInfo,
            mTextureCenter, mWorldTextureRadius, mCurrentPlayerPos, mTextureResolution
        );

        // Use PostDrawCallback which runs after the camera has rendered
        // This ensures the texture has valid data when we read it
        mRTTCamera->setPostDrawCallback(callback);

        Log(Debug::Info) << "[SNOW DIAGNOSTIC REWRITE] Readback callback attached";
    }
}
