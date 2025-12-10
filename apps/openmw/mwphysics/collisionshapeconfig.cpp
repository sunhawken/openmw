#include "collisionshapeconfig.hpp"

#include <algorithm>
#include <cctype>

#include <yaml-cpp/yaml.h>

#include <components/debug/debuglog.hpp>
#include <components/vfs/manager.hpp>

namespace MWPhysics
{
    bool CollisionShapeConfig::load(const VFS::Manager* vfs, const std::string& path)
    {
        if (!vfs->exists(path))
        {
            Log(Debug::Warning) << "Collision shape config not found: " << path;
            return false;
        }

        try
        {
            std::string rawYaml(std::istreambuf_iterator<char>(*vfs->get(path)), {});
            YAML::Node root = YAML::Load(rawYaml);

            if (!root.IsDefined() || root.IsNull() || root.IsScalar())
            {
                Log(Debug::Error) << "Invalid YAML in collision shape config: " << path;
                return false;
            }

            // Load pattern mappings
            if (root["patterns"])
            {
                for (const auto& entry : root["patterns"])
                {
                    if (entry["pattern"] && entry["shape"])
                    {
                        PatternEntry pe;
                        pe.pattern = entry["pattern"].as<std::string>();
                        pe.shape = parseShapeType(entry["shape"].as<std::string>());
                        mPatterns.push_back(pe);
                    }
                }
            }

            // Load exact item mappings
            if (root["items"])
            {
                for (const auto& entry : root["items"])
                {
                    std::string id = entry.first.as<std::string>();
                    std::string shapeStr = entry.second.as<std::string>();
                    mExactMappings[id] = parseShapeType(shapeStr);
                }
            }

            mLoaded = true;
            Log(Debug::Info) << "Loaded collision shape config with " << mPatterns.size()
                             << " patterns and " << mExactMappings.size() << " exact mappings";
            return true;
        }
        catch (const YAML::Exception& e)
        {
            Log(Debug::Error) << "Failed to parse collision shape config '" << path << "': " << e.what();
            return false;
        }
    }

    DynamicShapeType CollisionShapeConfig::getShapeType(const std::string& objectId) const
    {
        // First check exact mappings (case-insensitive)
        // Create lowercase version for comparison
        std::string lowerObjectId = objectId;
        std::transform(lowerObjectId.begin(), lowerObjectId.end(), lowerObjectId.begin(),
            [](unsigned char c) { return std::tolower(c); });

        for (const auto& [id, shape] : mExactMappings)
        {
            std::string lowerId = id;
            std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(),
                [](unsigned char c) { return std::tolower(c); });
            if (lowerId == lowerObjectId)
                return shape;
        }

        // Then check patterns (in order)
        for (const auto& pe : mPatterns)
        {
            if (matchesPattern(lowerObjectId, pe.pattern))
                return pe.shape;
        }

        // Default to box
        return DynamicShapeType::Box;
    }

    bool CollisionShapeConfig::matchesPattern(const std::string& id, const std::string& pattern)
    {
        // Simple wildcard matching with * (matches any sequence of characters)
        std::string lowerPattern = pattern;
        std::transform(lowerPattern.begin(), lowerPattern.end(), lowerPattern.begin(),
            [](unsigned char c) { return std::tolower(c); });

        size_t idPos = 0;
        size_t patPos = 0;
        size_t starPos = std::string::npos;
        size_t matchPos = 0;

        while (idPos < id.size())
        {
            if (patPos < lowerPattern.size() && (lowerPattern[patPos] == id[idPos] || lowerPattern[patPos] == '?'))
            {
                ++idPos;
                ++patPos;
            }
            else if (patPos < lowerPattern.size() && lowerPattern[patPos] == '*')
            {
                starPos = patPos++;
                matchPos = idPos;
            }
            else if (starPos != std::string::npos)
            {
                patPos = starPos + 1;
                idPos = ++matchPos;
            }
            else
            {
                return false;
            }
        }

        while (patPos < lowerPattern.size() && lowerPattern[patPos] == '*')
            ++patPos;

        return patPos == lowerPattern.size();
    }

    DynamicShapeType CollisionShapeConfig::parseShapeType(const std::string& typeStr)
    {
        std::string lower = typeStr;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return std::tolower(c); });

        if (lower == "sphere")
            return DynamicShapeType::Sphere;
        if (lower == "capsule")
            return DynamicShapeType::Capsule;
        if (lower == "cylinder")
            return DynamicShapeType::Cylinder;
        return DynamicShapeType::Box;
    }
}
