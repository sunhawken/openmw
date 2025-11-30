#ifndef OPENMW_COMPONENTS_TERRAIN_SNOWSIMULATION_H
#define OPENMW_COMPONENTS_TERRAIN_SNOWSIMULATION_H

#include <osg/Group>
#include <osg/Texture2D>
#include <osg/Camera>
#include <osg/Geode>
#include <osg/Uniform>

#include <memory>

namespace Resource
{
    class SceneManager;
}

namespace Terrain
{
    /// \brief Encapsulates the RTT simulation loop for snow deformation.
    /// Handles the Ping-Pong buffers, Update Shader, and Blur Passes.
    class SnowSimulation : public osg::Group
    {
    public:
        SnowSimulation(Resource::SceneManager* sceneManager, osg::Texture2D* objectMask);
        
        /// Update the simulation (scrolling, decay, swapping buffers)
        void update(float dt, const osg::Vec3f& centerPos);

        /// Get the final result (Blurred Deformation Map)
        osg::Texture2D* getOutputTexture() const { return mBlurredDeformationMap.get(); }
        
        /// Get the current center of the simulation in world space
        const osg::Vec3f& getCenter() const { return mCenter; }
        
        /// Get the size of the simulation area in world units
        float getSize() const { return mSize; }

    private:
        void initRTT(osg::Texture2D* objectMask);
        void createUpdatePass(osg::Texture2D* objectMask);
        void createBlurPasses();

        Resource::SceneManager* mSceneManager;

        // Simulation State
        float mSize; // World units (e.g., 50m)
        osg::Vec3f mCenter;
        osg::Vec3f mPreviousCenter;
        bool mFirstFrame;
        int mWriteBufferIndex;

        // Textures
        osg::ref_ptr<osg::Texture2D> mAccumulationMap[2]; // Ping-Pong buffers
        osg::ref_ptr<osg::Texture2D> mBlurTempBuffer;
        osg::ref_ptr<osg::Texture2D> mBlurredDeformationMap;

        // Cameras & Geometry
        osg::ref_ptr<osg::Camera> mUpdateCamera;
        osg::ref_ptr<osg::Geode> mUpdateQuad;
        
        osg::ref_ptr<osg::Camera> mBlurHCamera;
        osg::ref_ptr<osg::Geode> mBlurHQuad;
        
        osg::ref_ptr<osg::Camera> mBlurVCamera;
        osg::ref_ptr<osg::Geode> mBlurVQuad;

        // Uniforms
        osg::ref_ptr<osg::Uniform> mRTTOffsetUniform;
        osg::ref_ptr<osg::Uniform> mDecayUniform;
        osg::ref_ptr<osg::Uniform> mFirstFrameUniform;
    };
}

#endif
