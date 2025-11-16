#include "subdivisiontracker.hpp"
#include <components/debug/debuglog.hpp>
#include <cmath>
#include <algorithm>

namespace Terrain
{
    SubdivisionTracker::SubdivisionTracker()
        : mMaxTrailTime(120.0f)      // 2 minutes - chunks stay subdivided for this long
        , mMaxTrailDistance(3072.0f)  // ~375 meters - maximum trail distance
        , mDecayStartTime(30.0f)      // Subdivision starts reducing after 30 seconds
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

            // Calculate distance from player to chunk (world space)
            float distance = (playerPos - data.chunkCenter).length();

            // Update timers
            // Check if player is beyond subdivision range (no longer maintaining this chunk actively)
            if (distance > 1536.0f)  // Beyond max subdivision range
            {
                data.timeSincePlayerLeft += dt;
            }
            else
            {
                // Player is nearby, reset the "left" timer and keep tracking
                data.timeSincePlayerLeft = 0.0f;
                data.timeSubdivided += dt;
            }

            // Check if chunk should be removed from tracking
            if (!shouldMaintainSubdivision(data, distance))
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
        // Keep subdivision if:
        // 1. Player is still nearby (within trail distance), OR
        // 2. Not enough time has passed since player left

        if (distanceFromPlayer <= mMaxTrailDistance)
            return true;  // Player still in range

        if (data.timeSincePlayerLeft < mMaxTrailTime)
            return true;  // Trail time not expired

        return false;  // Release subdivision
    }

    int SubdivisionTracker::calculateDecayedLevel(const ChunkSubdivisionData& data) const
    {
        // Gradually reduce subdivision level over time after player leaves

        if (data.timeSincePlayerLeft < mDecayStartTime)
        {
            // Keep original level during grace period
            return data.subdivisionLevel;
        }

        // After decay start time, gradually reduce level
        float decayProgress = (data.timeSincePlayerLeft - mDecayStartTime) / (mMaxTrailTime - mDecayStartTime);
        decayProgress = std::min(1.0f, std::max(0.0f, decayProgress));

        // Reduce level based on decay progress
        // Level 2 -> Level 1 at 33% decay, Level 1 -> Level 0 at 66% decay
        int reducedLevels = static_cast<int>(decayProgress * data.subdivisionLevel);
        int currentLevel = std::max(0, data.subdivisionLevel - reducedLevels);

        return currentLevel;
    }

    int SubdivisionTracker::getSubdivisionLevel(const osg::Vec2f& chunkCenter, float distance) const
    {
        // Calculate distance-based subdivision level first
        // Add 64 unit buffer for early subdivision (pre-subdivision before entering chunk)
        const float SUBDIVISION_BUFFER = 64.0f;  // ~8 meters pre-subdivision buffer

        int distanceBasedLevel = 0;
        if (distance < 256.0f + SUBDIVISION_BUFFER)
            distanceBasedLevel = 3;  // Very high detail (directly under player)
        else if (distance < 768.0f + SUBDIVISION_BUFFER)
            distanceBasedLevel = 2;  // High detail
        else if (distance < 1536.0f + SUBDIVISION_BUFFER)
            distanceBasedLevel = 1;  // Medium detail

        // Check if chunk is being tracked (has been subdivided before)
        auto key = chunkToKey(chunkCenter);
        auto it = mTrackedChunks.find(key);

        if (it != mTrackedChunks.end())
        {
            const ChunkSubdivisionData& data = it->second;

            // Calculate current level with decay
            int trackedLevel = calculateDecayedLevel(data);

            // Use the HIGHER of tracked level or distance-based level
            // This ensures chunks maintain their subdivision when you return to them
            // but also get proper subdivision when you first approach
            int finalLevel = std::max(trackedLevel, distanceBasedLevel);

            if (finalLevel > 0)
            {
                return finalLevel;
            }
        }

        // Not tracked, just use distance-based level
        return distanceBasedLevel;
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
