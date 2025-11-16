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

    /// Manages the snow deformation system
    /// Handles RTT, footprint stamping, and deformation texture management
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

        /// Enable/disable debug visualization overlay
        void setDebugVisualization(bool enabled);

    private:
        /// Initialize RTT camera and deformation textures
        void setupRTT(osg::Group* rootNode);

        /// Create ping-pong deformation textures
        void createDeformationTextures();

        /// Create footprint stamping geometry and shaders
        void setupFootprintStamping();

        /// Update RTT camera position to follow player
        void updateCameraPosition(const osg::Vec3f& playerPos);

        /// Render a footprint into the deformation texture
        void renderFootprint(const osg::Vec3f& worldPos);

        /// Setup debug HUD camera
        void setupDebugHUD(osg::Group* rootNode);
        void updateDebugHUD();

        Resource::SceneManager* mSceneManager;
        Storage* mTerrainStorage;
        ESM::RefId mWorldspace;
        bool mEnabled;
        bool mActive;  // Currently active (player on snow)

        // RTT setup
        osg::ref_ptr<osg::Camera> mRTTCamera;
        osg::ref_ptr<osg::Texture2D> mDeformationTexture[2];  // Ping-pong buffers
        int mCurrentTextureIndex;

        // Deformation texture parameters
        int mTextureResolution;        // Texture size (512 or 1024)
        float mWorldTextureRadius;     // World space coverage radius
        osg::Vec2f mTextureCenter;     // Current center in world space

        // Footprint parameters
        float mFootprintRadius;        // Footprint radius in world units
        float mFootprintInterval;      // Distance between footprints
        float mDeformationDepth;       // Maximum deformation depth
        osg::Vec3f mLastFootprintPos;  // Last position where footprint was stamped
        float mTimeSinceLastFootprint; // Time accumulator

        // Footprint rendering
        osg::ref_ptr<osg::Group> mFootprintGroup;  // Group for footprint geometry
        osg::ref_ptr<osg::Geometry> mFootprintQuad;
        osg::ref_ptr<osg::StateSet> mFootprintStateSet;

        // Debug HUD
        osg::ref_ptr<osg::Camera> mDebugHUDCamera;
        osg::ref_ptr<osg::Geometry> mDebugQuad;
        bool mDebugVisualization;

        // Game time for decay
        float mCurrentTime;
    };
}

#endif
