#ifndef OPENMW_COMPONENTS_TERRAIN_SNOWDEFORMATION_H
#define OPENMW_COMPONENTS_TERRAIN_SNOWDEFORMATION_H

#include <osg/Vec3f>
#include <osg/Vec2f>
#include <osg/Uniform>

#include <components/esm/refid.hpp>

#include <deque>
#include <vector>
#include <string>

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
    /// - Footprints stored as Vec3(X, Y, timestamp)
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

    private:
        /// Stamp a new footprint at player position
        void stampFootprint(const osg::Vec3f& position);

        /// Update shader uniforms from footprint array
        void updateShaderUniforms();

        /// Update terrain-specific parameters
        void updateTerrainParameters(const osg::Vec3f& playerPos);

        /// Detect terrain texture at position
        std::string detectTerrainTexture(const osg::Vec3f& worldPos);

        Resource::SceneManager* mSceneManager;
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
    };
}

#endif
