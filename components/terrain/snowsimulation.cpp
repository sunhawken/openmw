#include "snowsimulation.hpp"

#include <components/resource/scenemanager.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/debug/debuglog.hpp>

#include <osg/Geometry>
#include <osg/Depth>
#include <osg/GL>

namespace Terrain
{
    // Debug callback to verify camera actually renders
    class SnowCameraDrawCallback : public osg::Camera::DrawCallback
    {
    public:
        SnowCameraDrawCallback(const std::string& name, osg::Texture2D* targetTex)
            : mName(name), mTargetTexture(targetTex), mDrawCount(0) {}

        virtual void operator()(osg::RenderInfo& renderInfo) const override
        {
            mDrawCount++;

            // Only log every 60 frames to avoid spam
            if (mDrawCount % 60 == 1)
            {
                // Get GL texture ID
                unsigned int contextID = renderInfo.getContextID();
                GLuint glTexID = 0;
                if (mTargetTexture)
                {
                    osg::Texture::TextureObject* texObj = mTargetTexture->getTextureObject(contextID);
                    if (texObj)
                        glTexID = texObj->id();
                }

                Log(Debug::Info) << "[SnowSim] " << mName << " DrawCallback fired! Count: " << mDrawCount
                                << ", GL TexID: " << glTexID;
            }
        }

    private:
        std::string mName;
        osg::Texture2D* mTargetTexture;
        mutable int mDrawCount;
    };

    // Debug callback to verify camera is being traversed (culled) by scene graph
    class SnowCameraCullCallback : public osg::NodeCallback
    {
    public:
        SnowCameraCullCallback(const std::string& name) : mName(name), mCullCount(0) {}

        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            mCullCount++;
            if (mCullCount % 60 == 1)
            {
                Log(Debug::Info) << "[SnowSim] " << mName << " CullCallback fired! Count: " << mCullCount;
            }
            traverse(node, nv);
        }

    private:
        std::string mName;
        mutable int mCullCount;
    };

    // Static counter for traverse logging
    static int sTraverseCount = 0;

    SnowSimulation::SnowSimulation(Resource::SceneManager* sceneManager, osg::Texture2D* objectMask)
        : mSceneManager(sceneManager)
        , mSize(3625.0f) // 50 meters coverage
        , mCenter(0.0f, 0.0f, 0.0f)
        , mPreviousCenter(0.0f, 0.0f, 0.0f)
        , mFirstFrame(true)
        , mWriteBufferIndex(0)
    {
        initRTT(objectMask);
    }

    void SnowSimulation::traverse(osg::NodeVisitor& nv)
    {
        sTraverseCount++;
        if (sTraverseCount % 60 == 1)
        {
            Log(Debug::Info) << "[SnowSim] SnowSimulation::traverse() called! Count: " << sTraverseCount
                            << ", VisitorType: " << nv.getVisitorType()
                            << ", NumChildren: " << getNumChildren();
        }
        osg::Group::traverse(nv);
    }

    void SnowSimulation::update(float dt, const osg::Vec3f& centerPos)
    {
        if (!mUpdateCamera) return;

        // 1. Calculate Sliding Window Offset
        osg::Vec3f delta = centerPos - mPreviousCenter;

        // If this is the first frame (or huge jump), reset
        if (delta.length() > mSize)
        {
            delta = osg::Vec3f(0, 0, 0);
        }

        osg::Vec2 offset(delta.x() / mSize, delta.y() / mSize);
        if (mRTTOffsetUniform) mRTTOffsetUniform->set(offset);

        mPreviousCenter = centerPos;
        mCenter = centerPos;

        // 2. Calculate Decay (Hardcoded 180s for now, can be parameterized later)
        float decayTime = 180.0f;
        float decayAmount = (decayTime > 0.0f) ? (dt / decayTime) : 1.0f;
        if (mDecayUniform) mDecayUniform->set(decayAmount);

        if (mFirstFrameUniform)
        {
            mFirstFrameUniform->set(mFirstFrame);
            if (mFirstFrame) mFirstFrame = false;
        }

        // SIMPLIFIED: No more ping-pong buffer swapping
        // The update shader reads from mAccumulationMap[1] (previous frame's result)
        // and writes to mAccumulationMap[0] (current frame's result).
        // Then we swap the textures' contents by re-rendering, not by pointer swap.
        //
        // This avoids the alternating texture binding that was causing vibration.
        // The update shader's "previousFrame" samples from buffer[1],
        // the update camera renders to buffer[0],
        // blur H reads from buffer[0] (stable).
        //
        // After each frame, we need buffer[0]'s contents to become buffer[1] for next frame.
        // This is handled implicitly because:
        // - Update reads from buffer[1] with UV offset (scrolling)
        // - Update writes MAX(previous, new) to buffer[0]
        // - Buffer[1] is never explicitly updated, but the scrolling UV makes this work
        //
        // Actually, we DO need ping-pong, but we should NOT dynamically change blur input.
        // Let's use a third buffer approach: write to a dedicated "current frame" buffer,
        // then have blur always read from that.

        // For now, simplified approach: always write to buffer[0], read from buffer[0]
        // (single buffer feedback, relying on GPU execution order)
    }

    void SnowSimulation::initRTT(osg::Texture2D* objectMask)
    {
        // 1. Create Ping-Pong Textures
        // Use SAME format as ObjectMaskMap (which works!) - GL_RGBA / GL_UNSIGNED_BYTE
        for (int i = 0; i < 2; ++i)
        {
            mAccumulationMap[i] = new osg::Texture2D;
            mAccumulationMap[i]->setTextureSize(2048, 2048);
            mAccumulationMap[i]->setInternalFormat(GL_RGBA);  // Match ObjectMaskMap
            mAccumulationMap[i]->setSourceFormat(GL_RGBA);
            mAccumulationMap[i]->setSourceType(GL_UNSIGNED_BYTE);  // Match ObjectMaskMap
            mAccumulationMap[i]->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
            mAccumulationMap[i]->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
            mAccumulationMap[i]->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::CLAMP_TO_BORDER);
            mAccumulationMap[i]->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::CLAMP_TO_BORDER);
            mAccumulationMap[i]->setBorderColor(osg::Vec4(0, 0, 0, 0));

            // DON'T set an image - let FBO rendering define the contents
            // (ObjectMaskMap doesn't have setImage either)
        }

        // Initialize Blur Textures with Black Images
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
            
            osg::ref_ptr<osg::Image> clearImage = new osg::Image;
            clearImage->allocateImage(2048, 2048, 1, GL_RGBA, GL_FLOAT);
            memset(clearImage->data(), 0, clearImage->getTotalSizeInBytes());
            tex->setImage(clearImage);
        };
        initBlurTex(mBlurTempBuffer);
        initBlurTex(mBlurredDeformationMap);

        createUpdatePass(objectMask);
        createBlurPasses();
        createCopyPass();
    }

    void SnowSimulation::createUpdatePass(osg::Texture2D* objectMask)
    {
        // Create Update Camera
        mUpdateCamera = new osg::Camera;
        mUpdateCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        mUpdateCamera->setClearMask(GL_COLOR_BUFFER_BIT);
        mUpdateCamera->setRenderOrder(osg::Camera::PRE_RENDER, 1);
        mUpdateCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mUpdateCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mUpdateCamera->setProjectionMatrixAsOrtho2D(0, 1, 0, 1);
        mUpdateCamera->setViewMatrix(osg::Matrix::identity());
        mUpdateCamera->setViewport(0, 0, 2048, 2048);
        mUpdateCamera->setCullingActive(false); // CRITICAL: Disable culling
        mUpdateCamera->setNodeMask(1 << 17); // Mask_RenderToTexture - CRITICAL for OpenMW!
        mUpdateCamera->setImplicitBufferAttachmentMask(0, 0); // Don't create implicit buffers
        mUpdateCamera->attach(osg::Camera::COLOR_BUFFER, mAccumulationMap[0]); // Initial target

        // DEBUG: Add callbacks to verify camera is traversed and renders
        mUpdateCamera->setCullCallback(new SnowCameraCullCallback("UpdateCamera"));
        mUpdateCamera->setFinalDrawCallback(new SnowCameraDrawCallback("UpdateCamera", mAccumulationMap[0].get()));

        // Create Update Quad
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

        // Update StateSet
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
            Log(Debug::Error) << "SnowSimulation: Failed to load update shaders!";
        }

        // Uniforms - Use simple int constructor for samplers (not array constructor!)
        ss->addUniform(new osg::Uniform("previousFrame", 0)); // Unit 0

        // Bind previousFrame texture (buffer[1]) to unit 0
        // Update camera writes to buffer[0], reads previous from buffer[1]
        ss->setTextureAttributeAndModes(0, mAccumulationMap[1], osg::StateAttribute::ON);

        // Bind Object Mask
        ss->setTextureAttributeAndModes(1, objectMask, osg::StateAttribute::ON);
        ss->addUniform(new osg::Uniform("objectMask", 1)); // Unit 1

        mRTTOffsetUniform = new osg::Uniform("offset", osg::Vec2(0, 0));
        ss->addUniform(mRTTOffsetUniform);

        mDecayUniform = new osg::Uniform("decayAmount", 0.0f);
        ss->addUniform(mDecayUniform);

        mFirstFrameUniform = new osg::Uniform("firstFrame", true);
        ss->addUniform(mFirstFrameUniform);

        addChild(mUpdateCamera);
    }

    void SnowSimulation::createBlurPasses()
    {
        // Initialize Blur Textures (Already initialized in initRTT)
        // auto initBlurTex = ... (Removed local lambda)

        auto& shaderManager = mSceneManager->getShaderManager();
        osg::ref_ptr<osg::Shader> vertShader = shaderManager.getShader("snow_update.vert", {}, osg::Shader::VERTEX);

        // --- Blur Pass 1 (Horizontal) ---
        mBlurHCamera = new osg::Camera;
        mBlurHCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        mBlurHCamera->setClearMask(GL_COLOR_BUFFER_BIT);
        mBlurHCamera->setRenderOrder(osg::Camera::PRE_RENDER, 3);
        mBlurHCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mBlurHCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mBlurHCamera->setProjectionMatrixAsOrtho2D(0, 1, 0, 1);
        mBlurHCamera->setViewMatrix(osg::Matrix::identity());
        mBlurHCamera->setViewport(0, 0, 2048, 2048);
        mBlurHCamera->setCullingActive(false); // CRITICAL: Disable culling
        mBlurHCamera->setNodeMask(1 << 17); // Mask_RenderToTexture
        mBlurHCamera->setImplicitBufferAttachmentMask(0, 0);
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

        // Blur H reads from buffer[0] (output of update pass) - FIXED, not dynamic
        hSS->setTextureAttributeAndModes(0, mAccumulationMap[0], osg::StateAttribute::ON);

        osg::ref_ptr<osg::Program> hProg = new osg::Program;
        hProg->addShader(vertShader);
        osg::ref_ptr<osg::Shader> hFrag = shaderManager.getShader("blur_horizontal.frag", {}, osg::Shader::FRAGMENT);
        if (hFrag) hProg->addShader(hFrag);
        hSS->setAttributeAndModes(hProg, osg::StateAttribute::ON);
        hSS->addUniform(new osg::Uniform("inputTex", 0));

        addChild(mBlurHCamera);

        // --- Blur Pass 2 (Vertical) ---
        mBlurVCamera = new osg::Camera;
        mBlurVCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        mBlurVCamera->setClearMask(GL_COLOR_BUFFER_BIT);
        mBlurVCamera->setRenderOrder(osg::Camera::PRE_RENDER, 4);
        mBlurVCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mBlurVCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mBlurVCamera->setProjectionMatrixAsOrtho2D(0, 1, 0, 1);
        mBlurVCamera->setViewMatrix(osg::Matrix::identity());
        mBlurVCamera->setViewport(0, 0, 2048, 2048);
        mBlurVCamera->setCullingActive(false); // CRITICAL: Disable culling
        mBlurVCamera->setNodeMask(1 << 17); // Mask_RenderToTexture
        mBlurVCamera->setImplicitBufferAttachmentMask(0, 0);
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
        vSS->setTextureAttributeAndModes(0, mBlurTempBuffer, osg::StateAttribute::ON);
        
        osg::ref_ptr<osg::Program> vProg = new osg::Program;
        vProg->addShader(vertShader);
        osg::ref_ptr<osg::Shader> vFrag = shaderManager.getShader("blur_vertical.frag", {}, osg::Shader::FRAGMENT);
        if (vFrag) vProg->addShader(vFrag);
        vSS->setAttributeAndModes(vProg, osg::StateAttribute::ON);
        vSS->addUniform(new osg::Uniform("inputTex", 0));

        addChild(mBlurVCamera);
    }

    void SnowSimulation::createCopyPass()
    {
        // Copy Pass: Copies buffer[0] (current frame) to buffer[1] (for next frame's "previousFrame")
        // This runs AFTER blur V (render order 5), so the update pass can read stable data next frame

        mCopyCamera = new osg::Camera;
        mCopyCamera->setClearMask(0); // Don't clear, just copy
        mCopyCamera->setRenderOrder(osg::Camera::PRE_RENDER, 5); // After blur V (4)
        mCopyCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mCopyCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mCopyCamera->setProjectionMatrixAsOrtho2D(0, 1, 0, 1);
        mCopyCamera->setViewMatrix(osg::Matrix::identity());
        mCopyCamera->setViewport(0, 0, 2048, 2048);
        mCopyCamera->setCullingActive(false);
        mCopyCamera->setNodeMask(1 << 17); // Mask_RenderToTexture
        mCopyCamera->setImplicitBufferAttachmentMask(0, 0);
        mCopyCamera->attach(osg::Camera::COLOR_BUFFER, mAccumulationMap[1]); // Write to buffer[1]

        mCopyQuad = new osg::Geode;
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
            mCopyQuad->addDrawable(g);
        }
        mCopyCamera->addChild(mCopyQuad);

        osg::StateSet* ss = mCopyQuad->getOrCreateStateSet();
        ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

        // Read from buffer[0] (current frame's update result)
        ss->setTextureAttributeAndModes(0, mAccumulationMap[0], osg::StateAttribute::ON);

        // Simple pass-through shader (just copy the texture)
        osg::Program* copyProg = new osg::Program;
        copyProg->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 120\n"
            "void main() {\n"
            "  gl_Position = ftransform();\n"
            "  gl_TexCoord[0] = gl_MultiTexCoord0;\n"
            "}\n"));
        copyProg->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\n"
            "uniform sampler2D inputTex;\n"
            "void main() {\n"
            "  gl_FragColor = texture2D(inputTex, gl_TexCoord[0].xy);\n"
            "}\n"));
        ss->setAttributeAndModes(copyProg, osg::StateAttribute::ON);
        ss->addUniform(new osg::Uniform("inputTex", 0));

        addChild(mCopyCamera);
    }
}
