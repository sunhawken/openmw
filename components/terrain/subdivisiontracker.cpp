#include "subdivisiontracker.hpp"
#include <components/debug/debuglog.hpp>
#include <cmath>
#include <algorithm>

namespace Terrain
{
    SubdivisionTracker::SubdivisionTracker()
        : mMaxTrailTime(60.0f)       // 60 seconds - chunks stay subdivided for this long
        , mMaxTrailDistance(3072.0f)  // Kept for compatibility but not used in time-based decay
        , mDecayStartTime(10.0f)      // Subdivision starts reducing after 10 seconds
    {
    }

    std::pair<int, int> SubdivisionTracker::chunkToKey(const osg::Vec2f& center) const
    {
        // Round to nearest 0.01 to avoid floating point precision issues
        int x = static_cast<int>(std::round(center.x() * 100.0f));
        int y = static_cast<int>(std::round(center.y() * 100.0f));
        return std::make_pair(x, y);
    }

    void SubdivisionTracker::update(float dt, const osg::Vec2f& playerPos)
    {
        // Update all tracked chunks and remove expired ones
        auto it = mTrackedChunks.begin();
        while (it != mTrackedChunks.end())
        {
            ChunkSubdivisionData& data = it->second;

            // Calculate grid distance from player to chunk
            // Use Chebyshev distance (max of absolute differences) for grid-based logic
            // Chunk world size is assumed to be 256 units
            const float CHUNK_WORLD_SIZE = 256.0f;
            int playerChunkX = static_cast<int>(std::floor(playerPos.x() / CHUNK_WORLD_SIZE));
            int playerChunkY = static_cast<int>(std::floor(playerPos.y() / CHUNK_WORLD_SIZE));

            int chunkGridX = static_cast<int>(std::floor(data.chunkCenter.x() / CHUNK_WORLD_SIZE));
            int chunkGridY = static_cast<int>(std::floor(data.chunkCenter.y() / CHUNK_WORLD_SIZE));

            int gridDeltaX = std::abs(chunkGridX - playerChunkX);
            int gridDeltaY = std::abs(chunkGridY - playerChunkY);
            int gridDistance = std::max(gridDeltaX, gridDeltaY);

            // Update timers based on grid distance
            // Check if player is beyond 5x5 grid (gridDistance > 2)
            if (gridDistance > 2)  // Beyond level 2 subdivision range (outside 5x5 grid)
            {
                data.timeSincePlayerLeft += dt;
            }
            else
            {
                // Player is within 5x5 grid, reset the "left" timer and keep tracking
                data.timeSincePlayerLeft = 0.0f;
                data.timeSubdivided += dt;
            }

            // Check if chunk should be removed from tracking (time-based only)
            if (data.timeSincePlayerLeft >= mMaxTrailTime)
            {
                it = mTrackedChunks.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    bool SubdivisionTracker::shouldMaintainSubdivision(const ChunkSubdivisionData& data, float distanceFromPlayer) const
    {
        // Time-based trail only: chunks maintain subdivision based purely on time since player left
        // No distance checks - this creates a pure time-based trail system
        return data.timeSincePlayerLeft < mMaxTrailTime;
    }

    int SubdivisionTracker::calculateDecayedLevel(const ChunkSubdivisionData& data) const
    {
        // Gradually reduce subdivision level over time after player leaves
        // Timeline (from CHUNK_SUBDIVISION_SYSTEM.md):
        // 0-10 sec:   Full subdivision (grace period)
        // 10-35 sec:  Level 3 → 2 (first half of decay)
        // 35-60 sec:  Level 2 → 0 (second half of decay)

        float timeSinceLeft = data.timeSincePlayerLeft;

        if (timeSinceLeft < mDecayStartTime)
        {
            // Grace period: keep original level
            return data.subdivisionLevel;
        }

        // Grid-based system: only levels 0, 2, and 3 exist
        if (data.subdivisionLevel == 3)
        {
            if (timeSinceLeft < 35.0f)
                return 3;  // 10-35 sec: stay at level 3
            else if (timeSinceLeft < mMaxTrailTime)
                return 2;  // 35-60 sec: drop to level 2
            else
                return 0;  // 60+ sec: fully decayed
        }
        else if (data.subdivisionLevel == 2)
        {
            if (timeSinceLeft < mMaxTrailTime)
                return 2;  // Stay at level 2 until 60 seconds
            else
                return 0;  // 60+ sec: fully decayed
        }

        return 0;
    }

    int SubdivisionTracker::getSubdivisionLevel(const osg::Vec2f& chunkCenter, float distance) const
    {
        // GRID-BASED subdivision (not distance-based circles!)
        // Creates predictable 3x3 and 5x5 rectangular patterns centered on player
        //
        // From CHUNK_SUBDIVISION_SYSTEM.md:
        // - 3x3 inner grid: 9 chunks at level 3 (player + 8 adjacent chunks)
        // - 5x5 total grid: 25 chunks total, outer 16 at level 2
        // - Pattern moves with player - always centered

        // Note: chunkCenter is in CELL coordinates (e.g., 0.5, 1.5)
        // We need to work in chunk grid coordinates to get the proper grid pattern

        // Since chunks have size 1.0 in cell coordinates for size=1.0 chunks,
        // we can round the chunk center to get grid coordinates
        // For example: chunkCenter=(0.5, 0.5) is chunk (0,0), chunkCenter=(1.5, 0.5) is chunk (1,0)
        int chunkGridX = static_cast<int>(std::round(chunkCenter.x()));
        int chunkGridY = static_cast<int>(std::round(chunkCenter.y()));

        int gridBasedLevel = 0;

        // Check if chunk is being tracked (has been subdivided before)
        auto key = chunkToKey(chunkCenter);
        auto it = mTrackedChunks.find(key);

        if (it != mTrackedChunks.end())
        {
            const ChunkSubdivisionData& data = it->second;

            // For tracked chunks, we need to know the player's grid position
            // But we only have the world center stored. We can use distance as a fallback
            // for now, or we could pass player position to this function.

            // Calculate current level with decay
            int trackedLevel = calculateDecayedLevel(data);

            // For tracked chunks in the trail system, use the decayed level
            // This maintains subdivision for the trail effect
            if (trackedLevel > 0)
            {
                gridBasedLevel = trackedLevel;
            }
        }

        // Note: The grid-based level is primarily calculated in getSubdivisionLevelFromPlayerGrid
        // This function is called with a pre-calculated distance which may not fully capture
        // the grid concept. The proper fix is to pass player grid position here.
        // For now, we rely on the calling code to provide correct grid-based distance.

        return gridBasedLevel;
    }

    int SubdivisionTracker::getSubdivisionLevelFromPlayerGrid(const osg::Vec2f& chunkCenter, const osg::Vec2f& playerWorldPos, float cellSize) const
    {
        // TRUE GRID-BASED subdivision using Chebyshev distance (creates square grids)
        //
        // This is the correct implementation matching CHUNK_SUBDIVISION_SYSTEM.md requirements:
        // - Creates predictable rectangular 3x3 and 5x5 patterns
        // - Always centered on player's current chunk
        // - Pattern moves with player

        // COORDINATE SYSTEM CLARIFICATION:
        // - chunkCenter is in CELL coordinates where size=1.0 means one full cell
        //   Example: chunkCenter=(0.5, 0.5) means center is at 0.5 cells = 0.5*8192 = 4096 world units
        // - playerWorldPos is in WORLD coordinates (actual position in meters)
        // - cellSize is the size of ONE CELL in world units (8192 for Morrowind, 4096 for ESM4)
        //
        // For size=1.0 chunks (which is what we're dealing with):
        // - chunkCenter values like 0.5, 1.5, 2.5, etc. represent chunk centers
        // - Each chunk is 1.0 cell units = 8192 world units wide
        //
        // To get grid coordinates, we need to floor both the player and chunk positions
        // when converted to the same coordinate system (cell units)

        // Player position in cell coordinates
        float playerCellX = playerWorldPos.x() / cellSize;
        float playerCellY = playerWorldPos.y() / cellSize;

        // Convert to integer grid positions (which chunk each is in)
        int playerChunkX = static_cast<int>(std::floor(playerCellX));
        int playerChunkY = static_cast<int>(std::floor(playerCellY));

        int chunkGridX = static_cast<int>(std::floor(chunkCenter.x()));
        int chunkGridY = static_cast<int>(std::floor(chunkCenter.y()));

        // Calculate Chebyshev distance (max of absolute differences)
        // This creates square patterns instead of circular distance-based zones
        int gridDeltaX = std::abs(chunkGridX - playerChunkX);
        int gridDeltaY = std::abs(chunkGridY - playerChunkY);
        int gridDistance = std::max(gridDeltaX, gridDeltaY);

        // Grid-based subdivision levels:
        // gridDistance <= 1: 3x3 grid (player's chunk + 1 in each direction) = Level 3
        // gridDistance <= 2: 5x5 grid (player's chunk + 2 in each direction) = Level 2
        // gridDistance > 2:  Outside grid = Level 0

        int gridBasedLevel = 0;

        if (gridDistance <= 1)
            gridBasedLevel = 3;  // 3x3 inner grid: 9 chunks at max detail
        else if (gridDistance <= 2)
            gridBasedLevel = 2;  // 5x5 outer ring: 16 chunks at medium detail

        // Check if chunk is being tracked (for trail system)
        auto key = chunkToKey(chunkCenter);
        auto it = mTrackedChunks.find(key);

        if (it != mTrackedChunks.end())
        {
            const ChunkSubdivisionData& data = it->second;

            // Calculate current level with decay
            int trackedLevel = calculateDecayedLevel(data);

            // Use the HIGHER of tracked level or grid-based level
            // This ensures chunks maintain their subdivision when you return to them
            // but also get proper subdivision when you first approach
            gridBasedLevel = std::max(trackedLevel, gridBasedLevel);
        }

        return gridBasedLevel;
    }

    void SubdivisionTracker::markChunkSubdivided(const osg::Vec2f& chunkCenter, int level, const osg::Vec2f& worldCenter)
    {
        if (level <= 0)
            return;  // Don't track non-subdivided chunks

        auto key = chunkToKey(chunkCenter);
        auto it = mTrackedChunks.find(key);

        if (it != mTrackedChunks.end())
        {
            // Update existing entry
            ChunkSubdivisionData& data = it->second;

            // Upgrade to higher level if needed
            if (level > data.subdivisionLevel)
            {
                data.subdivisionLevel = level;
            }

            // Reset timers since player is here
            data.timeSincePlayerLeft = 0.0f;
        }
        else
        {
            // Create new entry
            ChunkSubdivisionData data;
            data.subdivisionLevel = level;
            data.timeSubdivided = 0.0f;
            data.timeSincePlayerLeft = 0.0f;
            data.chunkCenter = worldCenter;  // Store WORLD coordinates for distance calculations

            mTrackedChunks[key] = data;
        }
    }

    void SubdivisionTracker::clear()
    {
        mTrackedChunks.clear();
    }
}
