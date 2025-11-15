#ifndef OPENMW_COMPONENTS_TERRAIN_SUBDIVISIONTRACKER_H
#define OPENMW_COMPONENTS_TERRAIN_SUBDIVISIONTRACKER_H

#include <map>
#include <osg/Vec2f>

namespace Terrain
{
    /// Tracks which chunks should remain subdivided to create a "trail" effect
    /// Chunks stay subdivided even after player leaves, creating visible snow paths
    class SubdivisionTracker
    {
    public:
        struct ChunkSubdivisionData
        {
            int subdivisionLevel;      // Current subdivision level (0-2)
            float timeSubdivided;      // How long this chunk has been subdivided (seconds)
            float timeSincePlayerLeft; // Time since player was last nearby (seconds)
            osg::Vec2f chunkCenter;    // Chunk position for distance calculations

            ChunkSubdivisionData()
                : subdivisionLevel(0)
                , timeSubdivided(0.0f)
                , timeSincePlayerLeft(0.0f)
                , chunkCenter(0.0f, 0.0f)
            {}
        };

        SubdivisionTracker();

        /// Update tracker each frame
        /// @param dt Delta time in seconds
        /// @param playerPos Current player position in world space
        void update(float dt, const osg::Vec2f& playerPos);

        /// Get the subdivision level for a chunk at given position
        /// @param chunkCenter Chunk center in cell coordinates
        /// @param distance Distance from player to chunk center in world units
        /// @return Subdivision level (0-2), or -1 if chunk should not be subdivided
        int getSubdivisionLevel(const osg::Vec2f& chunkCenter, float distance) const;

        /// Mark a chunk as subdivided (called when chunk is created with subdivision)
        /// @param chunkCenter Chunk center in cell coordinates (for key generation)
        /// @param level Subdivision level applied
        /// @param worldCenter Chunk center in world coordinates (for distance calculations)
        void markChunkSubdivided(const osg::Vec2f& chunkCenter, int level, const osg::Vec2f& worldCenter);

        /// Clear all tracked chunks (call when changing cells/worldspaces)
        void clear();

        /// Get number of currently tracked chunks
        size_t getTrackedChunkCount() const { return mTrackedChunks.size(); }

        /// Configuration
        void setMaxTrailTime(float seconds) { mMaxTrailTime = seconds; }
        void setMaxTrailDistance(float units) { mMaxTrailDistance = units; }
        void setDecayStartTime(float seconds) { mDecayStartTime = seconds; }

    private:
        /// Map of chunk centers to their subdivision data
        /// Key is chunk center rounded to avoid floating point precision issues
        std::map<std::pair<int, int>, ChunkSubdivisionData> mTrackedChunks;

        /// Maximum time a chunk stays subdivided after player leaves (seconds)
        float mMaxTrailTime;

        /// Maximum distance from player where chunks stay subdivided (world units)
        float mMaxTrailDistance;

        /// Time before subdivision starts decaying (seconds)
        float mDecayStartTime;

        /// Convert chunk center to integer key for map lookup
        std::pair<int, int> chunkToKey(const osg::Vec2f& center) const;

        /// Calculate if a chunk should still be subdivided based on time/distance
        bool shouldMaintainSubdivision(const ChunkSubdivisionData& data, float distanceFromPlayer) const;

        /// Decay subdivision level based on time since player left
        int calculateDecayedLevel(const ChunkSubdivisionData& data) const;
    };
}

#endif
