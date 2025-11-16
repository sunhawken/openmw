#ifndef OPENMW_COMPONENTS_TERRAIN_SNOWDEFORMATION_H
#define OPENMW_COMPONENTS_TERRAIN_SNOWDEFORMATION_H

#include <osg/Camera>
#include <osg/Texture2D>
#include <osg/Group>
#include <osg/Geometry>
#include <osg/Vec3f>
#include <osg/Vec2f>

#include <components/esm/refid.hpp>

namespace Resource
{
    class SceneManager;
}

namespace Terrain
{
    class Storage;

    /// ========================================================================
    /// SNOW TRAIL SYSTEM - Manager Class
    /// ========================================================================
    /// Manages the complete snow deformation and trail system
    ///
    /// FEATURES:
    /// - Non-additive trails: Multiple passes don't deepen snow
    /// - Time-based decay: Trails fade out over configurable time (default 3 min)
    /// - Age preservation: Walking on trails doesn't refresh decay timer
    /// - RTT-based rendering: Uses ping-pong buffers for accumulation
    /// - Dynamic following: Texture follows player with blit system
    ///
    /// TEXTURE CHANNELS:
    /// - R (Red):   Deformation depth (0.0 = no deformation, 1.0 = full depth)
    /// - G (Green): Age timestamp (game time when first deformed)
    /// - B (Blue):  Unused (reserved for future features)
    /// - A (Alpha): Always 1.0
    ///
    /// COORDINATES:
    /// - OpenMW uses Z-up coordinate system
    /// - Ground plane is XY, altitude is Z
    /// - Texture follows player on XY plane
    /// ========================================================================
    class SnowDeformationManager
    {
    public:
        SnowDeformationManager(
            Resource::SceneManager* sceneManager,
            Storage* terrainStorage,
            osg::Group* rootNode
        );
        ~SnowDeformationManager();

        /// Update deformation system each frame
        /// @param dt Delta time in seconds
        /// @param playerPos Current player position in world space
        void update(float dt, const osg::Vec3f& playerPos);

        /// Check if system should be active at this position
        /// @param worldPos Position to check
        /// @return True if player is on snow texture
        bool shouldBeActive(const osg::Vec3f& worldPos);

        /// Enable/disable the deformation system
        void setEnabled(bool enabled);
        bool isEnabled() const { return mEnabled; }

        /// Set current worldspace
        void setWorldspace(ESM::RefId worldspace);

        /// Get the current deformation texture for terrain shaders
        /// @return Texture containing deformation data, or nullptr if inactive
        osg::Texture2D* getDeformationTexture() const;

        /// Get deformation texture parameters for shader
        /// @param outCenter World-space center of deformation texture
        /// @param outRadius World-space radius covered by texture
        void getDeformationTextureParams(osg::Vec2f& outCenter, float& outRadius) const;

        /// Stamp a footprint at the current player position
        void stampFootprint(const osg::Vec3f& position);

        /// Get current deformation parameters (may vary by terrain texture)
        void getDeformationParams(float& outRadius, float& outDepth, float& outInterval) const;

        /// DIAGNOSTIC: Save current deformation texture to file for inspection
        /// filename: Output filename (e.g., "snow_deformation.png")
        /// debugInfo: If true, adds camera info overlay to saved image
        void saveDeformationTexture(const std::string& filename, bool debugInfo = true);

    private:
        /// Initialize RTT camera and deformation textures
        void setupRTT(osg::Group* rootNode);

        /// Create ping-pong deformation textures
        void createDeformationTextures();

        /// Create footprint stamping geometry and shaders
        void setupFootprintStamping();

        /// Setup blit system for texture scrolling
        void setupBlitSystem();

        /// Setup decay system for gradual snow restoration
        void setupDecaySystem();

        /// Update RTT camera position to follow player
        void updateCameraPosition(const osg::Vec3f& playerPos);

        /// Render a footprint into the deformation texture
        void renderFootprint(const osg::Vec3f& worldPos);

        /// Blit old texture content to new position when texture recenters
        void blitTexture(const osg::Vec2f& oldCenter, const osg::Vec2f& newCenter);

        /// Apply decay to the deformation texture
        void applyDecay(float dt);

        /// Update deformation parameters based on terrain texture at position
        void updateTerrainParameters(const osg::Vec3f& playerPos);

        /// Detect terrain texture at position
        std::string detectTerrainTexture(const osg::Vec3f& worldPos);

        Resource::SceneManager* mSceneManager;
        Storage* mTerrainStorage;
        ESM::RefId mWorldspace;
        bool mEnabled;
        bool mActive;  // Currently active (player on snow)

        // RTT setup
        osg::ref_ptr<osg::Camera> mRTTCamera;
        osg::ref_ptr<osg::Texture2D> mDeformationTexture[2];  // Ping-pong buffers
        int mCurrentTextureIndex;
        bool mTexturesInitialized;  // Track if textures have been cleared once

        // Deformation texture parameters
        int mTextureResolution;        // Texture size (512 or 1024)
        float mWorldTextureRadius;     // World space coverage radius
        osg::Vec2f mTextureCenter;     // Current center in world space

        // Footprint parameters
        float mFootprintRadius;        // Footprint radius in world units
        float mFootprintInterval;      // Distance between footprints
        float mDeformationDepth;       // Maximum deformation depth
        osg::Vec3f mLastFootprintPos;  // Last position where footprint was stamped
        osg::Vec3f mCurrentPlayerPos;  // Current player position (updated each frame)
        float mTimeSinceLastFootprint; // Time accumulator

        // Footprint rendering
        osg::ref_ptr<osg::Group> mFootprintGroup;  // Group for footprint geometry
        osg::ref_ptr<osg::Geometry> mFootprintQuad;
        osg::ref_ptr<osg::StateSet> mFootprintStateSet;

        // Blit system (for texture scrolling)
        osg::ref_ptr<osg::Group> mBlitGroup;
        osg::ref_ptr<osg::Geometry> mBlitQuad;
        osg::ref_ptr<osg::StateSet> mBlitStateSet;
        osg::Vec2f mLastBlitCenter;      // Last center position when blit was performed
        float mBlitThreshold;            // Distance player must move before blit (units)

        // Decay system (trail restoration)
        osg::ref_ptr<osg::Group> mDecayGroup;
        osg::ref_ptr<osg::Geometry> mDecayQuad;
        osg::ref_ptr<osg::StateSet> mDecayStateSet;
        float mDecayTime;                // TRAIL DECAY TIME: Seconds for full restoration (default 180s = 3 minutes)
        float mTimeSinceLastDecay;       // Accumulator for decay updates
        float mDecayUpdateInterval;      // How often to apply decay (default 0.1s for smooth restoration)

        // Texture-based parameters
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
    };
}

#endif
