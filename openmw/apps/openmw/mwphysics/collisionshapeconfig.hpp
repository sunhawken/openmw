#ifndef OPENMW_MWPHYSICS_COLLISIONSHAPECONFIG_H
#define OPENMW_MWPHYSICS_COLLISIONSHAPECONFIG_H

#include <string>
#include <unordered_map>
#include <vector>

namespace VFS
{
    class Manager;
}

namespace MWPhysics
{
    // Type of collision shape to use for dynamic objects
    enum class DynamicShapeType
    {
        Box,      // Axis-aligned bounding box (default)
        Sphere,   // Sphere (for round objects like gems, eggs)
        Capsule,  // Capsule (for elongated round objects like mushrooms)
        Cylinder  // Cylinder (for bottles, cups, bowls)
    };

    // Configuration for collision shape mappings loaded from YAML
    // Maps object IDs to their preferred collision shape type
    class CollisionShapeConfig
    {
    public:
        // Load configuration from a YAML file via VFS
        // Returns true if successfully loaded, false otherwise
        bool load(const VFS::Manager* vfs, const std::string& path);

        // Get the collision shape type for a given object ID
        // Falls back to Box if no mapping is found
        DynamicShapeType getShapeType(const std::string& objectId) const;

        // Check if configuration has been loaded
        bool isLoaded() const { return mLoaded; }

    private:
        // Pattern matching entry
        struct PatternEntry
        {
            std::string pattern;
            DynamicShapeType shape;
        };

        // Check if an ID matches a wildcard pattern (supports * wildcard)
        static bool matchesPattern(const std::string& id, const std::string& pattern);

        // Parse shape type string to enum
        static DynamicShapeType parseShapeType(const std::string& typeStr);

        // Exact ID -> shape type mappings
        std::unordered_map<std::string, DynamicShapeType> mExactMappings;

        // Pattern -> shape type mappings (evaluated in order)
        std::vector<PatternEntry> mPatterns;

        bool mLoaded = false;
    };
}

#endif
