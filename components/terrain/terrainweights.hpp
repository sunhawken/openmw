#ifndef COMPONENTS_TERRAIN_TERRAINWEIGHTS_H
#define COMPONENTS_TERRAIN_TERRAINWEIGHTS_H

#include <osg/Array>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/Image>

#include <components/esm/refid.hpp>

#include <vector>
#include <string>

namespace Terrain
{
    struct LayerInfo;
    class Storage;

    /// @brief Computes terrain deformation weights for vertices based on texture layers
    /// Uses LOD-based optimization to avoid expensive computations for distant terrain
    class TerrainWeights
    {
    public:
        /// LOD levels for weight computation optimization
        enum WeightLOD
        {
            /// Full per-vertex blendmap sampling (0-64m from player)
            LOD_FULL = 0,

            /// Single sample per chunk, all vertices get same weight (64-256m)
            LOD_SIMPLIFIED = 1,

            /// No deformable terrain, all rock (256m+)
            LOD_NONE = 2
        };

        /// Determine appropriate LOD level based on distance from player
        /// @param distanceToPlayer Distance from chunk center to player position
        /// @return LOD level for weight computation
        static WeightLOD determineLOD(float distanceToPlayer);

        /// Compute terrain weights for all vertices in a geometry
        /// @param vertices Vertex positions (chunk-local coordinates)
        /// @param chunkCenter Chunk center in cell units
        /// @param chunkSize Chunk size in cell units
        /// @param layerList Texture layers for this chunk
        /// @param blendmaps Blendmap images for this chunk
        /// @param terrainStorage Terrain storage for querying data
        /// @param worldspace Current worldspace
        /// @param playerPosition Player world position (for LOD calculation)
        /// @param cellWorldSize Size of one cell in world units
        /// @param lod LOD level for optimization
        /// @return Vec4Array with weights (x=snow, y=ash, z=mud, w=rock)
        static osg::ref_ptr<osg::Vec4Array> computeWeights(
            const osg::Vec3Array* vertices,
            const osg::Vec2f& chunkCenter,
            float chunkSize,
            const std::vector<LayerInfo>& layerList,
            const std::vector<osg::ref_ptr<osg::Image>>& blendmaps,
            Storage* terrainStorage,
            ESM::RefId worldspace,
            const osg::Vec3f& playerPosition,
            float cellWorldSize,
            WeightLOD lod);

        /// Compute weight for a single vertex (LEGACY - uses blendmaps)
        /// @deprecated Use the version that takes Storage* for chunk-boundary-consistent results
        /// @param vertexPos Vertex position in chunk-local coordinates
        /// @param chunkCenter Chunk center in cell units
        /// @param chunkSize Chunk size in cell units
        /// @param layerList Texture layers for this chunk
        /// @param blendmaps Blendmap images for this chunk
        /// @param cellWorldSize Size of one cell in world units
        /// @return Weight vector (x=snow, y=ash, z=mud, w=rock)
        static osg::Vec4f computeVertexWeight(
            const osg::Vec3f& vertexPos,
            const osg::Vec2f& chunkCenter,
            float chunkSize,
            const std::vector<LayerInfo>& layerList,
            const std::vector<osg::ref_ptr<osg::Image>>& blendmaps,
            float cellWorldSize);

        /// Compute weight for a single vertex (NEW - samples directly from land data)
        /// Uses world-coordinate-based land texture sampling for chunk-boundary consistency
        /// @param vertexPos Vertex position in chunk-local coordinates
        /// @param chunkCenter Chunk center in cell units
        /// @param terrainStorage Terrain storage for direct land data access
        /// @param worldspace Current worldspace
        /// @param cellWorldSize Size of one cell in world units
        /// @return Weight vector (x=snow, y=ash, z=mud, w=rock)
        static osg::Vec4f computeVertexWeightDirect(
            const osg::Vec3f& vertexPos,
            const osg::Vec2f& chunkCenter,
            Storage* terrainStorage,
            ESM::RefId worldspace,
            float cellWorldSize);

        /// Classify a texture path into terrain type and return its weight contribution
        /// @param texturePath Path to the texture (e.g., "textures/tx_snow_01.dds")
        /// @return Weight contribution (snow, ash, mud, rock)
        static osg::Vec4f classifyTexture(const std::string& texturePath);

        /// Sample blendmap at UV coordinates
        /// @param blendmap Blendmap image
        /// @param uv UV coordinates [0,1]
        /// @return Blend weight [0,1]
        static float sampleBlendmap(const osg::Image* blendmap, const osg::Vec2f& uv);

        /// Interpolate two weight vectors (used during triangle subdivision)
        /// @param w0 First weight vector
        /// @param w1 Second weight vector
        /// @return Interpolated weight (normalized)
        static osg::Vec4f interpolateWeights(const osg::Vec4f& w0, const osg::Vec4f& w1);

    private:
        // Distance thresholds for LOD levels (in world units)
        static constexpr float LOD_FULL_DISTANCE = 64.0f;
        static constexpr float LOD_SIMPLIFIED_DISTANCE = 256.0f;

        // Default weights for rock (no deformation)
        static const osg::Vec4f DEFAULT_ROCK_WEIGHT;

        // Cache for texture classification (avoid repeated string matching)
        // Not implemented yet - can be added if texture lookups become a bottleneck
    };
}

#endif
