#include "snowdetection.hpp"
#include "storage.hpp"

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Terrain
{
    std::vector<std::string> SnowDetection::sSnowPatterns;
    std::vector<std::string> SnowDetection::sAshPatterns;
    std::vector<std::string> SnowDetection::sMudPatterns;
    bool SnowDetection::sPatternsLoaded = false;

    void SnowDetection::loadSnowPatterns()
    {
        if (sPatternsLoaded)
            return;

        // Default patterns for common Morrowind snow textures
        sSnowPatterns = {
            "snow",
            "ice",
            "frost",
            "glacier",
            "tx_snow",
            "tx_bc_snow",
            "tx_ice",
            "bm_snow",      // Bloodmoon snow
            "bm_ice"        // Bloodmoon ice
        };

        // Ash texture patterns (Morrowind ash wastes)
        sAshPatterns = {
            "ash",
            "tx_ash",
            "tx_bc_ash",
            "tx_r_ash"
        };

        // Mud texture patterns
        sMudPatterns = {
            "mud",
            "swamp",
            "tx_mud",
            "tx_swamp",
            "tx_bc_mud"
        };

        sPatternsLoaded = true;
    }

    bool SnowDetection::isSnowTexture(const std::string& texturePath)
    {
        loadSnowPatterns();

        if (texturePath.empty())
            return false;

        // Convert to lowercase for case-insensitive comparison
        std::string lowerPath = texturePath;
        std::transform(lowerPath.begin(), lowerPath.end(),
                      lowerPath.begin(), ::tolower);

        // EXCLUSIONS FIRST: Check for textures that contain "snow" but aren't actually snow terrain
        // These are mixed textures (snow_grass, snow_rock, etc.) that should be classified
        // as their base terrain type, not as snow
        const std::vector<std::string> snowExclusions = {
            "snow_grass", "snowgrass",
            "snow_rock", "snowrock",
            "snow_dirt", "snowdirt",
            "snow_sand", "snowsand",
            "grass_snow", "grasssnow",
            "rock_snow", "rocksnow",
            "dirt_snow", "dirtsnow",
            "sand_snow", "sandsnow"
        };

        for (const auto& exclusion : snowExclusions)
        {
            if (lowerPath.find(exclusion) != std::string::npos)
            {
                return false; // This is a mixed texture, not pure snow
            }
        }

        // Check if any pattern matches
        for (const auto& pattern : sSnowPatterns)
        {
            if (lowerPath.find(pattern) != std::string::npos)
            {
                return true;
            }
        }

        return false;
    }

    bool SnowDetection::isAshTexture(const std::string& texturePath)
    {
        loadSnowPatterns();

        if (texturePath.empty())
            return false;

        std::string lowerPath = texturePath;
        std::transform(lowerPath.begin(), lowerPath.end(),
                      lowerPath.begin(), ::tolower);

        // EXCLUSIONS: Mixed ash textures should not be classified as pure ash
        const std::vector<std::string> ashExclusions = {
            "ash_grass", "ashgrass",
            "ash_rock", "ashrock",
            "grass_ash", "grassash",
            "rock_ash", "rockash"
        };

        for (const auto& exclusion : ashExclusions)
        {
            if (lowerPath.find(exclusion) != std::string::npos)
            {
                return false;
            }
        }

        for (const auto& pattern : sAshPatterns)
        {
            if (lowerPath.find(pattern) != std::string::npos)
            {
                return true;
            }
        }

        return false;
    }

    bool SnowDetection::isMudTexture(const std::string& texturePath)
    {
        loadSnowPatterns();

        if (texturePath.empty())
            return false;

        std::string lowerPath = texturePath;
        std::transform(lowerPath.begin(), lowerPath.end(),
                      lowerPath.begin(), ::tolower);

        // EXCLUSIONS: Mixed mud textures should not be classified as pure mud
        const std::vector<std::string> mudExclusions = {
            "mud_grass", "mudgrass",
            "mud_rock", "mudrock",
            "grass_mud", "grassmud",
            "rock_mud", "rockmud"
        };

        for (const auto& exclusion : mudExclusions)
        {
            if (lowerPath.find(exclusion) != std::string::npos)
            {
                return false;
            }
        }

        for (const auto& pattern : sMudPatterns)
        {
            if (lowerPath.find(pattern) != std::string::npos)
            {
                return true;
            }
        }

        return false;
    }

    SnowDetection::TerrainType SnowDetection::detectTerrainType(
        const osg::Vec3f& worldPos,
        Storage* terrainStorage,
        ESM::RefId worldspace)
    {
        // TESTING MODE: Always return Snow to enable deformation on all terrain
        // This allows testing the system without texture detection
        // TODO: Implement actual terrain texture querying when ready

        // Priority order: Snow > Ash > Mud
        // In the future, this will query the terrain storage for actual textures

        return TerrainType::Snow;  // Default for testing
    }

    bool SnowDetection::hasSnowAtPosition(
        const osg::Vec3f& worldPos,
        Storage* terrainStorage,
        ESM::RefId worldspace)
    {
        // TESTING MODE: Always return true to enable deformation on all terrain
        // This allows testing the system without texture detection
        // TODO: Implement actual snow texture detection when ready

        return true;  // Enable deformation everywhere for testing
    }

    float SnowDetection::sampleBlendMap(
        const osg::Image* blendmap,
        const osg::Vec2f& uv)
    {
        if (!blendmap || !blendmap->data())
            return 0.0f;

        // Clamp UV to [0, 1]
        float u = std::max(0.0f, std::min(1.0f, uv.x()));
        float v = std::max(0.0f, std::min(1.0f, uv.y()));

        // Convert to pixel coordinates
        int x = static_cast<int>(u * (blendmap->s() - 1));
        int y = static_cast<int>(v * (blendmap->t() - 1));

        // Clamp to image bounds
        x = std::max(0, std::min(x, blendmap->s() - 1));
        y = std::max(0, std::min(y, blendmap->t() - 1));

        // Get pixel data
        const unsigned char* pixel = blendmap->data(x, y);

        // Blendmaps typically store weight in alpha channel or as grayscale
        // Check the format and sample appropriately
        int bytesPerPixel = blendmap->getPixelSizeInBits() / 8;

        if (bytesPerPixel >= 4)
        {
            // RGBA format - use alpha channel
            return pixel[3] / 255.0f;
        }
        else if (bytesPerPixel >= 1)
        {
            // Grayscale - use red channel
            return pixel[0] / 255.0f;
        }

        return 0.0f;
    }

    const std::vector<std::string>& SnowDetection::getSnowPatterns()
    {
        loadSnowPatterns();
        return sSnowPatterns;
    }

    const std::vector<std::string>& SnowDetection::getAshPatterns()
    {
        loadSnowPatterns();
        return sAshPatterns;
    }

    const std::vector<std::string>& SnowDetection::getMudPatterns()
    {
        loadSnowPatterns();
        return sMudPatterns;
    }
}
