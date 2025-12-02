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
#include "snowsimulation.hpp"
#include "debugoverlay.hpp"

namespace Resource
{
    class SceneManager;
}

namespace Terrain
{
    class Storage;

    /// ========================================================================
    /// SNOW DEFORMATION SYSTEM - RTT Approach
    /// ========================================================================
    /// Persistent snow deformation using Render-To-Texture (RTT) and Ping-Pong Buffers
    ///
    /// HOW IT WORKS:
    /// - A Depth Camera renders actors (player, NPCs) from below into an Object Mask.
    /// - An Update Camera runs a shader (`snow_update.frag`) that:
    ///   1. Reads the previous frame's deformation map.
    ///   2. Applies "scrolling" based on player movement (sliding window).
    ///   3. Decays old deformation over time.
    ///   4. Adds new deformation where the Object Mask is white.
    /// - The result is written to a Ping-Pong buffer (Accumulation Map).
    /// - Two Blur Passes (Horizontal & Vertical) smooth the result.
    /// - The final Blurred Map is passed to the Terrain Shader for vertex displacement.
    ///
    /// ADVANTAGES:
    /// - Infinite trails (limited only by texture resolution/area).
    /// - Persistent deformation (until it decays).
    /// - Supports any object type (via Depth Camera).
    /// - Smooth results via Gaussian Blur.
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
        osg::Uniform* getDeformationDepthUniform() const { return mDeformationDepthUniform.get(); }
        osg::Uniform* getAshDeformationDepthUniform() const { return mAshDeformationDepthUniform.get(); }
        osg::Uniform* getMudDeformationDepthUniform() const { return mMudDeformationDepthUniform.get(); }
        osg::Uniform* getCurrentTimeUniform() const { return mCurrentTimeUniform.get(); }
        osg::Uniform* getDecayTimeUniform() const { return mDecayTimeUniform.get(); }

        // RTT Uniforms
        osg::Uniform* getDeformationMapUniform() const { return mDeformationMapUniform.get(); }
        osg::Texture2D* getDeformationMap() const { return mSimulation ? mSimulation->getOutputTexture() : nullptr; }
        osg::Texture2D* getCurrentDeformationMap() const { return mSimulation ? mSimulation->getOutputTexture() : nullptr; }
        osg::Uniform* getRTTWorldOriginUniform() const { return mRTTWorldOriginUniform.get(); }
        osg::Uniform* getRTTScaleUniform() const { return mRTTScaleUniform.get(); }

        // DEBUG: Expose internal textures for testing
        void debugDumpTexture(const std::string& filename, osg::Texture2D* texture) const;
        osg::Uniform* getObjectMaskUniform() const { return mObjectMaskUniform.get(); }
        osg::Texture2D* getObjectMaskMap() const { return mObjectMaskMap.get(); }
        osg::Texture2D* getAccumulationMap() const { return mSimulation ? mSimulation->getAccumulationMap() : nullptr; }

    private:
        /// Emit particles at position (renamed from stampFootprint)
        void emitParticles(const osg::Vec3f& position);

        /// Initialize RTT system
        void initRTT();

        /// Update RTT camera position and render footprints
        void updateRTT(float dt, const osg::Vec3f& playerPos);

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

        // Shader uniforms
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
            float cameraDepth;   // How much of body is captured (smaller = feet only)
            float blurSpread;    // Blur multiplier for edge smoothness
            std::string pattern;
        };
        std::vector<TerrainParams> mTerrainParams;
        std::string mCurrentTerrainType;
        float mCurrentCameraDepth;
        float mCurrentBlurSpread;

        // Game time
        float mCurrentTime;

        // Particle Emitter
        std::unique_ptr<SnowParticleEmitter> mParticleEmitter;

        // RTT System
        osg::ref_ptr<SnowSimulation> mSimulation;
        
        osg::ref_ptr<osg::Camera> mDepthCamera;  // Camera for rendering actors from below
        osg::ref_ptr<osg::Texture2D> mObjectMaskMap; // Mask of actors (White = Present)
        osg::ref_ptr<osg::Uniform> mObjectMaskUniform; // Uniform for update shader
        
        osg::ref_ptr<osg::Uniform> mDeformationMapUniform; // Points to the READ buffer (for terrain shader)
        
        osg::ref_ptr<osg::Uniform> mRTTWorldOriginUniform; // World position of RTT texture center
        osg::ref_ptr<osg::Uniform> mRTTScaleUniform;       // Scale of RTT area (meters)
        
        float mRTTSize; // Size of the RTT area in world units (e.g. 50m)
        osg::Vec3f mRTTCenter; // Current center of RTT area
        
        bool mFirstFrame; // Flag to reset accumulation on first frame
        
        osg::ref_ptr<DebugOverlay> mDebugOverlay;
    };
}

#endif
