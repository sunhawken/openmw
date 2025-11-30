#include "snowsimulation.hpp"

#include <components/resource/scenemanager.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/debug/debuglog.hpp>

#include <osg/Geometry>
#include <osg/Depth>

namespace Terrain
{
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

        // 3. Swap Buffers
        int readIndex = mWriteBufferIndex;
        mWriteBufferIndex = (mWriteBufferIndex + 1) % 2;
        int writeIndex = mWriteBufferIndex;

        // 4. Update Cameras to target new Write Buffer
        mUpdateCamera->attach(osg::Camera::COLOR_BUFFER, mAccumulationMap[writeIndex]);

        // 5. Update Shader to read from Read Buffer
        osg::StateSet* ss = mUpdateQuad->getStateSet();
        if (ss)
        {
            ss->setTextureAttributeAndModes(0, mAccumulationMap[readIndex], osg::StateAttribute::ON);
        }

        // 6. Update Blur H Input (Reads from Write Buffer of Update Pass)
        if (mBlurHQuad)
        {
            osg::StateSet* hSS = mBlurHQuad->getStateSet();
            if (hSS)
            {
                hSS->setTextureAttributeAndModes(0, mAccumulationMap[writeIndex], osg::StateAttribute::ON);
            }
        }
    }

    void SnowSimulation::initRTT(osg::Texture2D* objectMask)
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
            
            // Explicitly allocate with black pixels
            osg::ref_ptr<osg::Image> clearImage = new osg::Image;
            clearImage->allocateImage(2048, 2048, 1, GL_RGBA, GL_FLOAT);
            memset(clearImage->data(), 0, clearImage->getTotalSizeInBytes());
            mAccumulationMap[i]->setImage(clearImage);
        }

        createUpdatePass(objectMask);
        createBlurPasses();
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

        // Uniforms
        ss->addUniform(new osg::Uniform(osg::Uniform::SAMPLER_2D, "previousFrame", 0)); // Unit 0
        
        // Bind Object Mask
        ss->setTextureAttributeAndModes(1, objectMask, osg::StateAttribute::ON);
        ss->addUniform(new osg::Uniform(osg::Uniform::SAMPLER_2D, "objectMask", 1)); // Unit 1

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
}
