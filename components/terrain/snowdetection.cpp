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

        // TODO: Load additional patterns from settings if needed
        // std::string customPatterns = Settings::terrain().mSnowTexturePatterns;
        // Parse and add to sSnowPatterns

        sPatternsLoaded = true;

        Log(Debug::Info) << "[SNOW] Loaded " << sSnowPatterns.size() << " snow texture patterns";
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

    bool SnowDetection::hasSnowAtPosition(
        const osg::Vec3f& worldPos,
        Storage* terrainStorage,
        ESM::RefId worldspace)
    {
        // TESTING MODE: Always return true to enable deformation on all terrain
        // This allows testing the system without texture detection
        // TODO: Implement actual snow texture detection when ready

        Log(Debug::Verbose) << "[SNOW] Snow detection active at position ("
                           << (int)worldPos.x() << ", " << (int)worldPos.z() << ")";

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
}
