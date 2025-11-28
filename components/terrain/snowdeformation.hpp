#ifndef OPENMW_COMPONENTS_TERRAIN_SNOWDEFORMATION_H
#define OPENMW_COMPONENTS_TERRAIN_SNOWDEFORMATION_H

#include <osg/Vec3f>
#include <osg/Vec2f>
#include <osg/Uniform>
#include <osg/Texture2D>
#include <osg/Camera>
#include <osg/Geode>

#include <components/esm/refid.hpp>

#include <deque>
#include <vector>
#include <string>
#include <memory>

#include "snowparticleemitter.hpp"

namespace Resource
{
    class SceneManager;
}

namespace Terrain
{
    class Storage;

    /// ========================================================================
    /// SNOW DEFORMATION SYSTEM - Vertex Shader Array Approach
    /// ========================================================================
    /// Simple, efficient snow deformation using vertex shader displacement
    ///
    /// HOW IT WORKS:
    /// - Stores recent footprint positions in a CPU array (deque)
    /// - Passes positions to terrain vertex shader as uniform array
    /// - Shader loops through positions, applies deformation where close
    ///
    /// ADVANTAGES:
    /// - No RTT complexity (no cameras, FBOs, textures)
    /// - Direct integration with existing terrain shader
    /// - Fast to implement and debug
    /// - Works immediately without shader manager conflicts
    ///
    /// LIMITATIONS:
    /// - Trail length limited by shader uniform array size (~500 positions)
    /// - Trails don't persist across sessions (unless serialized)
    /// - Small vertex shader performance cost (negligible on modern GPUs)
    ///
    /// COORDINATES:
    /// - OpenMW uses Z-up coordinate system
    /// - Ground plane is XY, altitude is Z

    /// - Ground plane is XY, altitude is Z
    class SnowDeformationManager
    {
    public:
        SnowDeformationManager(Resource::SceneManager* sceneManager, Storage* terrainStorage, osg::Group* rootNode);
        ~SnowDeformationManager();

        void update(float dt, const osg::Vec3f& playerPos);

        /// Check if system should be active at this position
        bool shouldBeActive(const osg::Vec3f& worldPos);

        /// Enable/disable the deformation system
        void setEnabled(bool enabled);
        bool isEnabled() const { return mEnabled; }

        /// Set current worldspace
        void setWorldspace(ESM::RefId worldspace);

        /// Get shader uniforms for terrain rendering
        osg::Uniform* getFootprintPositionsUniform() const { return mFootprintPositionsUniform.get(); }
        osg::Uniform* getFootprintCountUniform() const { return mFootprintCountUniform.get(); }
        osg::Uniform* getFootprintRadiusUniform() const { return mFootprintRadiusUniform.get(); }
        osg::Uniform* getDeformationDepthUniform() const { return mDeformationDepthUniform.get(); }
        osg::Uniform* getAshDeformationDepthUniform() const { return mAshDeformationDepthUniform.get(); }
        osg::Uniform* getMudDeformationDepthUniform() const { return mMudDeformationDepthUniform.get(); }
        osg::Uniform* getCurrentTimeUniform() const { return mCurrentTimeUniform.get(); }
        osg::Uniform* getDecayTimeUniform() const { return mDecayTimeUniform.get(); }

        // RTT Uniforms
        osg::Uniform* getDeformationMapUniform() const { return mDeformationMapUniform.get(); }
        osg::Texture2D* getDeformationMap() const { return mBlurredDeformationMap.get(); } // Return BLURRED buffer
        osg::Texture2D* getCurrentDeformationMap() const { return mBlurredDeformationMap.get(); } // Alias for clarity
        osg::Uniform* getRTTWorldOriginUniform() const { return mRTTWorldOriginUniform.get(); }
        osg::Uniform* getRTTScaleUniform() const { return mRTTScaleUniform.get(); }

    private:
        /// Stamp a new footprint at player position
        void stampFootprint(const osg::Vec3f& position);

        /// Update shader uniforms from footprint array
        void updateShaderUniforms();

        /// Initialize RTT system
        void initRTT();

        /// Update RTT camera position and render footprints
        void updateRTT(float dt, const osg::Vec3f& playerPos);

        /// Create a footprint marker for RTT rendering
        void addFootprintToRTT(const osg::Vec3f& position, float rotation);

        /// Update terrain-specific parameters
        void updateTerrainParameters(const osg::Vec3f& playerPos);

        /// Detect terrain texture at position
        std::string detectTerrainTexture(const osg::Vec3f& worldPos);

        Resource::SceneManager* mSceneManager;
        osg::Group* mRootNode;
        Storage* mTerrainStorage;
        ESM::RefId mWorldspace;
        bool mEnabled;
        bool mActive;

        // Footprint storage (size configured via settings)
        std::deque<osg::Vec3f> mFootprints;  // Vec3(X, Y, timestamp)

        // Shader uniforms
        osg::ref_ptr<osg::Uniform> mFootprintPositionsUniform;     // vec3 array
        osg::ref_ptr<osg::Uniform> mFootprintCountUniform;         // int
        osg::ref_ptr<osg::Uniform> mFootprintRadiusUniform;        // float
        osg::ref_ptr<osg::Uniform> mDeformationDepthUniform;       // float (snow depth)
        osg::ref_ptr<osg::Uniform> mAshDeformationDepthUniform;    // float (ash depth)
        osg::ref_ptr<osg::Uniform> mMudDeformationDepthUniform;    // float (mud depth)
        osg::ref_ptr<osg::Uniform> mCurrentTimeUniform;            // float
        osg::ref_ptr<osg::Uniform> mDecayTimeUniform;              // float

        // Footprint parameters
        float mFootprintRadius;
        float mFootprintInterval;
        float mDeformationDepth;
        osg::Vec3f mLastFootprintPos;
        float mTimeSinceLastFootprint;

        // Decay parameters
        float mDecayTime;  // Time for trails to fully fade (default 180s)

        // Terrain-specific parameters
        struct TerrainParams {
            float radius;
            float depth;
            float interval;
            std::string pattern;
        };
        std::vector<TerrainParams> mTerrainParams;
        std::string mCurrentTerrainType;

        // Game time
        float mCurrentTime;

        // Particle Emitter
        std::unique_ptr<SnowParticleEmitter> mParticleEmitter;

        // RTT System
        osg::ref_ptr<osg::Texture2D> mAccumulationMap[2]; // Ping-Pong buffers
        int mWriteBufferIndex; // Index of the buffer we are currently writing to (0 or 1)
        
        osg::ref_ptr<osg::Camera> mUpdateCamera; // Camera for running the update shader
        osg::ref_ptr<osg::Geode> mUpdateQuad;    // Fullscreen quad for the update pass
        
        // Blur Pass 1 (Horizontal)
        osg::ref_ptr<osg::Camera> mBlurHCamera;
        osg::ref_ptr<osg::Geode> mBlurHQuad;
        osg::ref_ptr<osg::Texture2D> mBlurTempBuffer; // Intermediate buffer (R16F)

        // Blur Pass 2 (Vertical)
        osg::ref_ptr<osg::Camera> mBlurVCamera;
        osg::ref_ptr<osg::Geode> mBlurVQuad;
        osg::ref_ptr<osg::Texture2D> mBlurredDeformationMap; // Final blurred result (R16F)
        

        osg::ref_ptr<osg::Camera> mRTTCamera;    // Camera for rendering footprints
        osg::ref_ptr<osg::Group> mRTTScene;      // Scene graph for footprints

        osg::ref_ptr<osg::Camera> mDepthCamera;  // Camera for rendering actors from below
        osg::ref_ptr<osg::Texture2D> mObjectMaskMap; // Mask of actors (White = Present)
        osg::ref_ptr<osg::Uniform> mObjectMaskUniform; // Uniform for update shader
        
        osg::ref_ptr<osg::Uniform> mDeformationMapUniform; // Points to the READ buffer (for terrain shader)
        osg::ref_ptr<osg::Uniform> mPreviousFrameUniform;  // Points to the READ buffer (for update shader)
        osg::ref_ptr<osg::Uniform> mRTTOffsetUniform;      // UV offset for sliding window
        
        osg::ref_ptr<osg::Uniform> mRTTWorldOriginUniform; // World position of RTT texture center
        osg::ref_ptr<osg::Uniform> mRTTScaleUniform;       // Scale of RTT area (meters)
        
        float mRTTSize; // Size of the RTT area in world units (e.g. 50m)
        osg::Vec3f mRTTCenter; // Current center of RTT area
        osg::Vec3f mPreviousRTTCenter; // Center of RTT area in previous frame
    };
}

#endif
