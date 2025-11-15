#ifndef OPENMW_COMPONENTS_TERRAIN_SNOWDETECTION_H
#define OPENMW_COMPONENTS_TERRAIN_SNOWDETECTION_H

#include <string>
#include <vector>
#include <osg/Vec3f>
#include <osg/Vec2f>
#include <osg/Image>

#include <components/esm/refid.hpp>

namespace Terrain
{
    class Storage;

    /// Utilities for detecting snow textures at runtime
    /// Used to determine if snow deformation should be active
    class SnowDetection
    {
    public:
        /// Check if a texture filename indicates snow
        /// @param texturePath Texture filename or path
        /// @return True if texture appears to be snow/ice
        static bool isSnowTexture(const std::string& texturePath);

        /// Check if terrain at world position has snow texture
        /// @param worldPos Position in world space
        /// @param terrainStorage Terrain storage for layer queries
        /// @param worldspace Current worldspace ID
        /// @return True if standing on snow texture with sufficient blend weight
        static bool hasSnowAtPosition(
            const osg::Vec3f& worldPos,
            Storage* terrainStorage,
            ESM::RefId worldspace
        );

        /// Sample a blendmap to get texture weight at UV coordinate
        /// @param blendmap Blendmap image
        /// @param uv UV coordinates (0-1 range)
        /// @return Blend weight (0-1), 0 if blendmap is null
        static float sampleBlendMap(
            const osg::Image* blendmap,
            const osg::Vec2f& uv
        );

        /// Load snow texture patterns from settings
        /// Call once at startup
        static void loadSnowPatterns();

        /// Get the current snow texture patterns
        static const std::vector<std::string>& getSnowPatterns();

    private:
        static std::vector<std::string> sSnowPatterns;
        static bool sPatternsLoaded;
    };
}

#endif
