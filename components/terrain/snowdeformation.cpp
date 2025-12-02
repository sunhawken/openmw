#include "snowdeformation.hpp"
#include "snowdetection.hpp"
#include "storage.hpp"
#include <algorithm>

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/shader/shadermanager.hpp>

#include <osg/Texture2D>
#include <osg/FrameBufferObject>
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Depth>
#include <osg/BlendFunc>
#include <osg/BlendEquation>
#include <osg/NodeCallback>
#include <osg/NodeVisitor>
#include <osgUtil/CullVisitor>
#include <osgDB/WriteFile>

namespace Terrain
{
    // Callback to allow the Depth Camera to render the scene (siblings) 
    // without being a parent of the scene (which would cause a cycle).
    // Also filters out the Terrain itself to prevent self-deformation.
    class DepthCameraCullCallback : public osg::NodeCallback
    {
    public:
        DepthCameraCullCallback(osg::Group* root, osg::Camera* cam) 
            : mRoot(root)
            , mCam(cam) 
        {
        }

        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            traverse(node, nv);

            if (mRoot && nv)
            {
                int childrenTraversed = 0;
                int childrenSkipped = 0;
                
                // Log(Debug::Verbose) << "DepthCameraCullCallback: Visiting root " << mRoot << " (" << mRoot->getName() << ")";

                for (unsigned int i = 0; i < mRoot->getNumChildren(); ++i)
                {
                    osg::Node* child = mRoot->getChild(i);
                    std::string childName = child->getName().empty() ? "<unnamed>" : child->getName();

                    // CRITICAL: Skip other Cameras (RTT cameras) to prevent recursion/feedback
                    if (dynamic_cast<osg::Camera*>(child))
                    {
                        // Log(Debug::Verbose) << "  [SKIP] Camera: " << childName;
                        childrenSkipped++;
                        continue;
                    }

                    if (child == mCam)
                    {
                        // Log(Debug::Verbose) << "  [SKIP] Camera itself";
                        childrenSkipped++;
                        continue;
                    }

                    if (childName == "Terrain Root")
                    {
                        childrenSkipped++;
                        continue;
                    }

                    if (childName == "Sky Root")
                    {
                        // Log(Debug::Verbose) << "  [SKIP] Sky Root";
                        childrenSkipped++;
                        continue;
                    }

                    if (childName == "Water Root")
                    {
                        // Log(Debug::Verbose) << "  [SKIP] Water Root";
                        childrenSkipped++;
                        continue;
                    }

                    childrenTraversed++;
                    child->accept(*nv);
                }
                
            }
        }

    private:
        osg::Group* mRoot;
        osg::Camera* mCam;
    };

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
        , mCurrentCameraDepth(Settings::terrain().mSnowCameraDepth.get())
        , mCurrentBlurSpread(Settings::terrain().mSnowBlurSpread.get())
        , mCurrentTime(0.0f)
        , mRTTSize(3625.0f) // 50 meters coverage (approx 72.5 units/meter)
        , mRTTCenter(0.0f, 0.0f, 0.0f)
        , mFirstFrame(true)
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
                Settings::terrain().mSnowCameraDepth.get(),
                Settings::terrain().mSnowBlurSpread.get(),
                "snow"
            },
            {
                Settings::terrain().mAshFootprintRadius.get(),
                Settings::terrain().mAshDeformationDepth.get(),
                45.0f,  // interval
                Settings::terrain().mAshCameraDepth.get(),
                Settings::terrain().mAshBlurSpread.get(),
                "ash"
            },
            {
                Settings::terrain().mMudFootprintRadius.get(),
                Settings::terrain().mMudDeformationDepth.get(),
                45.0f,  // interval
                Settings::terrain().mMudCameraDepth.get(),
                Settings::terrain().mMudBlurSpread.get(),
                "mud"
            }
        };

        // Create shader uniforms
        // Note: Legacy footprint array uniforms removed
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

        // Only emit particles when actually moving (distance check only, no time-based emission)
        // Minimum movement threshold to avoid particles when standing still
        const float minMovementForParticles = 5.0f; // ~3 inches of movement

        if (distanceMoved > mFootprintInterval)
        {
            // Only emit particles if we're actually moving, not for mud, and occasionally
            bool shouldEmitParticles = (distanceMoved > minMovementForParticles)
                                       && (mCurrentTerrainType != "mud");

            if (shouldEmitParticles)
            {
                Log(Debug::Verbose) << "SnowDeformationManager::update - Emitting particles at " << playerPos;
                emitParticles(playerPos);
            }

            mLastFootprintPos = playerPos;
            mTimeSinceLastFootprint = 0.0f;
        }

        // Update current time uniform
        mCurrentTimeUniform->set(mCurrentTime);

        // Update RTT
        if (mRootNode)
        {
            // Log(Debug::Verbose) << "SnowDeformationManager::update - RootNode Children: " << mRootNode->getNumChildren();
        }
        updateRTT(dt, playerPos);
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
            }
        }
    }

    void SnowDeformationManager::setWorldspace(ESM::RefId worldspace)
    {
        mWorldspace = worldspace;
    }

    void SnowDeformationManager::emitParticles(const osg::Vec3f& position)
    {
        Log(Debug::Verbose) << "SnowDeformationManager::emitParticles - Pos: " << position << ", Z: " << position.z();
        
        // Emit particles
        if (mParticleEmitter)
        {
            mParticleEmitter->emit(position, mCurrentTerrainType);
        }
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
                mCurrentCameraDepth = params.cameraDepth;
                mCurrentBlurSpread = params.blurSpread;

                // Update simulation with new blur spread
                if (mSimulation)
                {
                    mSimulation->setBlurSpread(mCurrentBlurSpread);
                }

                Log(Debug::Info) << "Terrain type changed to: " << terrainType
                                << " (cameraDepth=" << mCurrentCameraDepth
                                << ", blurSpread=" << mCurrentBlurSpread << ")";
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
        // 1. Create Object Mask Map & Camera (Pass 0: Render Actors)
        mObjectMaskMap = new osg::Texture2D;
        mObjectMaskMap->setTextureSize(2048, 2048);
        mObjectMaskMap->setInternalFormat(GL_RGBA); // Use RGBA for safety
        mObjectMaskMap->setSourceFormat(GL_RGBA);
        mObjectMaskMap->setSourceType(GL_UNSIGNED_BYTE);
        mObjectMaskMap->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
        mObjectMaskMap->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
        mObjectMaskMap->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::CLAMP_TO_BORDER);
        mObjectMaskMap->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::CLAMP_TO_BORDER);
        mObjectMaskMap->setBorderColor(osg::Vec4(0, 0, 0, 0));

        // Create Depth Texture for FBO completeness
        osg::Texture2D* depthTex = new osg::Texture2D;
        depthTex->setTextureSize(2048, 2048);
        depthTex->setInternalFormat(GL_DEPTH_COMPONENT24);
        depthTex->setSourceFormat(GL_DEPTH_COMPONENT);
        depthTex->setSourceType(GL_FLOAT);
        depthTex->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::NEAREST);
        depthTex->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::NEAREST);

        mDepthCamera = new osg::Camera;
        // Clear to BLACK (0.0) - No object
        mDepthCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f)); 
        mDepthCamera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        mDepthCamera->setRenderOrder(osg::Camera::PRE_RENDER, 0); // Run FIRST
        mDepthCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mDepthCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF); // Absolute Frame
        mDepthCamera->setCullingActive(false); // CRITICAL: Don't cull this camera (it has no children)
        mDepthCamera->setViewport(0, 0, 2048, 2048);
        mDepthCamera->attach(osg::Camera::COLOR_BUFFER, mObjectMaskMap);
        mDepthCamera->attach(osg::Camera::DEPTH_BUFFER, depthTex); // Attach Depth Buffer

        // Cull Mask: Actor(3) | Player(4) | Object(10)
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
        dss->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        dss->setMode(GL_TEXTURE_2D, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

        // 2. Create Simulation
        mSimulation = new SnowSimulation(mSceneManager, mObjectMaskMap);

        // Add cameras to scene graph
        if (mRootNode)
        {
            mRootNode->addChild(mDepthCamera);
            mRootNode->addChild(mSimulation);

            // SOLUTION: Attach CullCallback to allow depth camera to see scene without circular reference
            mDepthCamera->setCullCallback(new DepthCameraCullCallback(mRootNode, mDepthCamera));
            Log(Debug::Info) << "SnowDeformationManager: Attached DepthCameraCullCallback to depth camera";
        }
        else
        {
            Log(Debug::Error) << "SnowDeformationManager: Root node is null, RTT will not update!";
        }
        
        // 3. Create Uniforms for Terrain
        mDeformationMapUniform = new osg::Uniform(osg::Uniform::SAMPLER_2D, "snowDeformationMap");
        mDeformationMapUniform->set(7); // Texture unit 7
        
        mRTTWorldOriginUniform = new osg::Uniform("snowRTTWorldOrigin", osg::Vec3f(0,0,0));
        mRTTScaleUniform = new osg::Uniform("snowRTTScale", mRTTSize);

        // 4. Create Debug Overlay
        mDebugOverlay = new DebugOverlay(1920, 1080); // Assuming 1080p for now
        
        // Add textures to overlay
        // Size: 256x256
        float size = 256.0f;
        float gap = 10.0f;
        float startX = (1920.0f - (size * 3 + gap * 2)) / 2.0f;
        
        mDebugOverlay->addTexture(mObjectMaskMap, startX, 10, size, size, "Object Mask");
        mDebugOverlay->addTexture(mSimulation->getAccumulationMap(), startX + size + gap, 10, size, size, "Accumulation");
        mDebugOverlay->addTexture(mSimulation->getOutputTexture(), startX + (size + gap) * 2, 10, size, size, "Output");
        
        if (mRootNode)
        {
            mRootNode->addChild(mDebugOverlay);
        }

    }

    void SnowDeformationManager::updateRTT(float dt, const osg::Vec3f& playerPos)
    {
        if (!mSimulation) return;

        mRTTCenter = playerPos;
        mRTTWorldOriginUniform->set(mRTTCenter);
        
        // Update Depth Camera View/Projection
        if (mDepthCamera)
        {
            float halfSize = mRTTSize * 0.5f;

            // Camera depth controls how much of the body is captured:
            // - Small values (50-100): Only feet touch the ground (good for mud, ash)
            // - Large values (200-500): Full body sinks into terrain (good for deep snow)
            float nearPlane = 1.0f;
            float farPlane = nearPlane + mCurrentCameraDepth;

            // Ortho Projection centered on player
            mDepthCamera->setProjectionMatrixAsOrtho(
                -halfSize, halfSize,
                -halfSize, halfSize,
                nearPlane, farPlane);

            // View Matrix: Top-Down looking at player
            // Eye position is at farPlane distance above ground
            osg::Vec3f eye = mRTTCenter + osg::Vec3f(0, 0, farPlane);
            osg::Vec3f center = mRTTCenter;
            osg::Vec3f up = osg::Vec3f(0, 1, 0);

            mDepthCamera->setViewMatrixAsLookAt(eye, center, up);
        }

        // DEBUG: Dump Object Mask
        static int dumpCounter = 0;
        if (dumpCounter++ % 600 == 0) // Dump every 600 frames (approx 10s)
        {
             debugDumpTexture("object_mask_dump.png", mObjectMaskMap.get());
        }

        // Update Simulation
        // Log(Debug::Verbose) << "SnowDeformationManager::updateRTT - Updating simulation";
        mSimulation->update(dt, playerPos);
    }

    void SnowDeformationManager::debugDumpTexture(const std::string& filename, osg::Texture2D* texture) const
    {
        if (!texture) return;

        osg::Image* image = texture->getImage();
        if (!image)
        {
            // Texture might not have an image attached (FBO target)
            // We need to read it from GPU
            image = new osg::Image;
            image->allocateImage(
                texture->getTextureWidth(),
                texture->getTextureHeight(),
                1,
                GL_RGBA,
                GL_UNSIGNED_BYTE);

            // This requires a valid OpenGL context, call during rendering
            Log(Debug::Warning) << "Cannot dump texture without GPU readback - needs implementation";
            return;
        }

        std::string fullPath = "d:\\Gamedev\\OpenMW\\openmw-dev-master\\" + filename;
        if (osgDB::writeImageFile(*image, fullPath))
        {
            Log(Debug::Info) << "DEBUG: Dumped texture to " << fullPath;
        }
        else
        {
            Log(Debug::Error) << "DEBUG: Failed to dump texture to " << fullPath;
        }
    }

    // =========================================================================================
    // DebugOverlay Implementation (Moved here to avoid linker errors if file not added to project)
    // =========================================================================================

    DebugOverlay::DebugOverlay(int width, int height)
    {
        setProjectionMatrixAsOrtho2D(0, width, 0, height);
        setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        setViewMatrix(osg::Matrix::identity());
        setClearMask(0); // Don't clear, just draw on top
        setRenderOrder(osg::Camera::POST_RENDER, 10000); // Draw last
        setAllowEventFocus(false);
        setCullingActive(false); // CRITICAL: Disable culling so it's always drawn

        mGeode = new osg::Geode;
        osg::StateSet* ss = mGeode->getOrCreateStateSet();
        ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        
        addChild(mGeode);
    }

    void DebugOverlay::addTexture(osg::Texture2D* texture, float x, float y, float w, float h, const std::string& label)
    {
        if (!texture) return;

        osg::Geometry* geom = new osg::Geometry;
        
        osg::Vec3Array* verts = new osg::Vec3Array;
        verts->push_back(osg::Vec3(x, y, 0));
        verts->push_back(osg::Vec3(x + w, y, 0));
        verts->push_back(osg::Vec3(x + w, y + h, 0));
        verts->push_back(osg::Vec3(x, y + h, 0));
        geom->setVertexArray(verts);

        osg::Vec2Array* texcoords = new osg::Vec2Array;
        texcoords->push_back(osg::Vec2(0, 0));
        texcoords->push_back(osg::Vec2(1, 0));
        texcoords->push_back(osg::Vec2(1, 1));
        texcoords->push_back(osg::Vec2(0, 1));
        geom->setTexCoordArray(0, texcoords);

        geom->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, 4));

        osg::StateSet* ss = geom->getOrCreateStateSet();
        ss->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);

        mGeode->addDrawable(geom);
    }
}
