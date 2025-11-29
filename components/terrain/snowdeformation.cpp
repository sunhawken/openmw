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
                        // Log(Debug::Verbose) << "  [SKIP] Terrain Root";
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

                    // Log what we're about to traverse
                    unsigned int nodeMask = child->getNodeMask();
                    // Log(Debug::Verbose) << "  [TRAVERSE] " << childName
                    //                 << " (mask: 0x" << std::hex << nodeMask << std::dec << ")";
                    childrenTraversed++;
                    child->accept(*nv);
                }
                
                // Log(Debug::Verbose) << "DepthCameraCullCallback: Traversed " << childrenTraversed
                //                 << " nodes, skipped " << childrenSkipped;
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

        if (distanceMoved > mFootprintInterval || mTimeSinceLastFootprint > 0.5f)
        {
            Log(Debug::Verbose) << "SnowDeformationManager::update - Emitting particles at " << playerPos;
            emitParticles(playerPos);
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

                // Uniforms updated automatically via pointers or next frame
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

        // Initialize Blur Textures
        auto initBlurTex = [](osg::ref_ptr<osg::Texture2D>& tex) {
            tex = new osg::Texture2D;
            tex->setTextureSize(2048, 2048);
            tex->setInternalFormat(GL_RGBA16F_ARB);
            tex->setSourceFormat(GL_RGBA);
            tex->setSourceType(GL_FLOAT);
            tex->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
            tex->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
            tex->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::CLAMP_TO_EDGE);
            tex->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::CLAMP_TO_EDGE);
        };
        initBlurTex(mBlurTempBuffer);
        initBlurTex(mBlurredDeformationMap);

        // 2. Create Update Camera (Pass 1: Scroll & Decay & Apply New Deformation)
        mUpdateCamera = new osg::Camera;
        mUpdateCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f)); // Clear to no deformation
        mUpdateCamera->setClearMask(GL_COLOR_BUFFER_BIT); // Only clear color
        mUpdateCamera->setRenderOrder(osg::Camera::PRE_RENDER, 1); // Run AFTER Depth Camera
        mUpdateCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mUpdateCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mUpdateCamera->setProjectionMatrixAsOrtho2D(0, 1, 0, 1); // Normalized coordinates
        mUpdateCamera->setViewMatrix(osg::Matrix::identity());
        mUpdateCamera->setViewport(0, 0, 2048, 2048);
        mUpdateCamera->attach(osg::Camera::COLOR_BUFFER, mAccumulationMap[0]); // Initial target

        // Create Update Quad
        mUpdateQuad = new osg::Geode;
        osg::Geometry* geom = new osg::Geometry;
        osg::Vec3Array* verts = new osg::Vec3Array;
        verts->push_back(osg::Vec3(0, 0, 0));
        verts->push_back(osg::Vec3(1, 0, 0));
        verts->push_back(osg::Vec3(1, 1, 0));
        verts->push_back(osg::Vec3(0, 1, 0));
        geom->setVertexArray(verts);
        
        // DEBUG: Blue Color Array for Fixed Function
        // osg::Vec4Array* colors = new osg::Vec4Array;
        // colors->push_back(osg::Vec4(0, 0, 1, 1));
        // colors->push_back(osg::Vec4(0, 0, 1, 1));
        // colors->push_back(osg::Vec4(0, 0, 1, 1));
        // colors->push_back(osg::Vec4(0, 0, 1, 1));
        // geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);

        geom->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, 4));
        osg::Vec2Array* texcoords = new osg::Vec2Array;
        texcoords->push_back(osg::Vec2(0, 0));
        texcoords->push_back(osg::Vec2(1, 0));
        texcoords->push_back(osg::Vec2(1, 1));
        texcoords->push_back(osg::Vec2(0, 1));
        geom->setTexCoordArray(0, texcoords);
        mUpdateQuad->addDrawable(geom);
        mUpdateCamera->addChild(mUpdateQuad);

        // Update StateSet (Shader Based)
        osg::StateSet* ss = mUpdateQuad->getOrCreateStateSet();
        ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        
        // Load Update Shader
        auto& shaderManager = mSceneManager->getShaderManager();
        osg::ref_ptr<osg::Program> program = new osg::Program;
        osg::ref_ptr<osg::Shader> vertShader = shaderManager.getShader("snow_update.vert", {}, osg::Shader::VERTEX);
        osg::ref_ptr<osg::Shader> fragShader = shaderManager.getShader("snow_update.frag", {}, osg::Shader::FRAGMENT);

        if (vertShader && fragShader)
        {
            program->addShader(vertShader);
            program->addShader(fragShader);
            ss->setAttributeAndModes(program, osg::StateAttribute::ON);
        }
        else
        {
            Log(Debug::Error) << "SnowDeformationManager: Failed to load update shaders!";
        }

        // Keep uniforms for later restoration
        mPreviousFrameUniform = new osg::Uniform(osg::Uniform::SAMPLER_2D, "previousFrame");
        mPreviousFrameUniform->set(0); // Texture Unit 0
        ss->addUniform(mPreviousFrameUniform);

        mObjectMaskUniform = new osg::Uniform(osg::Uniform::SAMPLER_2D, "objectMask");
        mObjectMaskUniform->set(1); // Texture Unit 1
        ss->addUniform(mObjectMaskUniform);

        mRTTOffsetUniform = new osg::Uniform("offset", osg::Vec2(0, 0));
        ss->addUniform(mRTTOffsetUniform);
        
        osg::Uniform* decayUniform = new osg::Uniform("decayAmount", 0.0f);
        ss->addUniform(decayUniform);

        // --- Create Blur Pass 1 (Horizontal) ---
        mBlurHCamera = new osg::Camera;
        mBlurHCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f)); // Clear to BLACK (no deformation)
        mBlurHCamera->setClearMask(GL_COLOR_BUFFER_BIT);
        mBlurHCamera->setRenderOrder(osg::Camera::PRE_RENDER, 3); // Order 3 (After Footprints)
        mBlurHCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mBlurHCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mBlurHCamera->setProjectionMatrixAsOrtho2D(0, 1, 0, 1);
        mBlurHCamera->setViewMatrix(osg::Matrix::identity());
        mBlurHCamera->setViewport(0, 0, 2048, 2048);
        mBlurHCamera->attach(osg::Camera::COLOR_BUFFER, mBlurTempBuffer);

        mBlurHQuad = new osg::Geode;
        {
            osg::Geometry* g = new osg::Geometry;
            osg::Vec3Array* v = new osg::Vec3Array;
            v->push_back(osg::Vec3(0, 0, 0)); v->push_back(osg::Vec3(1, 0, 0));
            v->push_back(osg::Vec3(1, 1, 0)); v->push_back(osg::Vec3(0, 1, 0));
            g->setVertexArray(v);
            g->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, 4));
            osg::Vec2Array* t = new osg::Vec2Array;
            t->push_back(osg::Vec2(0, 0)); t->push_back(osg::Vec2(1, 0));
            t->push_back(osg::Vec2(1, 1)); t->push_back(osg::Vec2(0, 1));
            g->setTexCoordArray(0, t);
            mBlurHQuad->addDrawable(g);
        }
        mBlurHCamera->addChild(mBlurHQuad);

        osg::StateSet* hSS = mBlurHQuad->getOrCreateStateSet();
        hSS->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        hSS->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        
        osg::ref_ptr<osg::Program> hProg = new osg::Program;
        // Reuse snow_update.vert as it's just a pass-through
        hProg->addShader(vertShader);
        osg::ref_ptr<osg::Shader> hFrag = shaderManager.getShader("blur_horizontal.frag", {}, osg::Shader::FRAGMENT);
        if (hFrag) hProg->addShader(hFrag);
        else Log(Debug::Error) << "Failed to load blur_horizontal.frag";
        hSS->setAttributeAndModes(hProg, osg::StateAttribute::ON);
        hSS->addUniform(new osg::Uniform("inputTex", 0)); // Unit 0

        // --- Create Blur Pass 2 (Vertical) ---
        mBlurVCamera = new osg::Camera;
        mBlurVCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f)); // Clear to BLACK (no deformation)
        mBlurVCamera->setClearMask(GL_COLOR_BUFFER_BIT);
        mBlurVCamera->setRenderOrder(osg::Camera::PRE_RENDER, 4); // Order 4
        mBlurVCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mBlurVCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mBlurVCamera->setProjectionMatrixAsOrtho2D(0, 1, 0, 1);
        mBlurVCamera->setViewMatrix(osg::Matrix::identity());
        mBlurVCamera->setViewport(0, 0, 2048, 2048);
        mBlurVCamera->attach(osg::Camera::COLOR_BUFFER, mBlurredDeformationMap);

        mBlurVQuad = new osg::Geode;
        {
            osg::Geometry* g = new osg::Geometry;
            osg::Vec3Array* v = new osg::Vec3Array;
            v->push_back(osg::Vec3(0, 0, 0)); v->push_back(osg::Vec3(1, 0, 0));
            v->push_back(osg::Vec3(1, 1, 0)); v->push_back(osg::Vec3(0, 1, 0));
            g->setVertexArray(v);
            g->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, 4));
            osg::Vec2Array* t = new osg::Vec2Array;
            t->push_back(osg::Vec2(0, 0)); t->push_back(osg::Vec2(1, 0));
            t->push_back(osg::Vec2(1, 1)); t->push_back(osg::Vec2(0, 1));
            g->setTexCoordArray(0, t);
            mBlurVQuad->addDrawable(g);
        }
        mBlurVCamera->addChild(mBlurVQuad);

        osg::StateSet* vSS = mBlurVQuad->getOrCreateStateSet();
        vSS->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        vSS->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        vSS->setTextureAttributeAndModes(0, mBlurTempBuffer, osg::StateAttribute::ON); // Always reads from Temp Buffer

        osg::ref_ptr<osg::Program> vProg = new osg::Program;
        vProg->addShader(vertShader);
        osg::ref_ptr<osg::Shader> vFrag = shaderManager.getShader("blur_vertical.frag", {}, osg::Shader::FRAGMENT);
        if (vFrag) vProg->addShader(vFrag);
        else Log(Debug::Error) << "Failed to load blur_vertical.frag";
        vSS->setAttributeAndModes(vProg, osg::StateAttribute::ON);
        vSS->addUniform(new osg::Uniform("inputTex", 0));

        // 3. Create Object Mask Map & Camera (Pass 0: Render Actors)
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
        
        ss->setTextureAttributeAndModes(1, mObjectMaskMap, osg::StateAttribute::ON); // Bind Object Mask to Unit 1

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

        // Add cameras to scene graph
        if (mRootNode)
        {
            mRootNode->addChild(mDepthCamera);
            mRootNode->addChild(mUpdateCamera);
            // mRTTCamera removed (Legacy)
            mRootNode->addChild(mBlurHCamera);
            mRootNode->addChild(mBlurVCamera);

            // SOLUTION: Attach CullCallback to allow depth camera to see scene without circular reference
            // The callback manually traverses mRootNode's children during the depth camera's cull pass
            // This allows the camera to render actors while being a sibling in the scene graph
            mDepthCamera->setCullCallback(new DepthCameraCullCallback(mRootNode, mDepthCamera));
            Log(Debug::Info) << "SnowDeformationManager: Attached DepthCameraCullCallback to depth camera";
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

    void SnowDeformationManager::updateRTT(float dt, const osg::Vec3f& playerPos)
    {
        if (!mUpdateCamera) return;

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
        
        mRTTCenter = playerPos;
        mRTTWorldOriginUniform->set(mRTTCenter);
        
        Log(Debug::Verbose) << "SnowDeformationManager::updateRTT - Center: " << mRTTCenter << ", Scale: " << mRTTSize;

        // Update Depth Camera View/Projection
        if (mDepthCamera)
        {
            float halfSize = mRTTSize * 0.5f;
            // Ortho Projection centered on player
            mDepthCamera->setProjectionMatrixAsOrtho(
                -halfSize, halfSize, 
                -halfSize, halfSize, 
                1.0f, 500.0f); // Near/Far planes (adjust as needed)

            // View Matrix: Top-Down looking at player
            // Eye: PlayerPos + Up * 100
            // Center: PlayerPos
            // Up: Y-axis (Standard Top-Down)
            osg::Vec3f eye = mRTTCenter + osg::Vec3f(0, 0, 200.0f);
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

        // 2. Calculate Decay
        // We want to decay by dt / decayTime
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
        
        Log(Debug::Verbose) << "SnowDeformationManager::updateRTT - Swapped buffers. Read: " << readIndex << ", Write: " << writeIndex;
        
        // 5. Update Shader to read from Read Buffer
        if (ss)
        {
            ss->setTextureAttributeAndModes(0, mAccumulationMap[readIndex], osg::StateAttribute::ON);
        }
        
        // Update Blur H Input (Reads from Write Buffer of Update Pass)
        // Blur H (Order 3) reads mAccumulationMap[writeIndex].
        if (mBlurHQuad)
        {
            osg::StateSet* hSS = mBlurHQuad->getStateSet();
            if (hSS)
            {
                hSS->setTextureAttributeAndModes(0, mAccumulationMap[writeIndex], osg::StateAttribute::ON);
            }
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
        
        // 8. Render NEW Footprints (Legacy - Removed)
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
}
