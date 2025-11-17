#ifndef COMPONENTS_TERRAIN_TERRAINSUBDIVIDER_H
#define COMPONENTS_TERRAIN_TERRAINSUBDIVIDER_H

#include <osg/Geometry>
#include <osg/Vec2>
#include <osg/Vec3>
#include <osg/Vec4>
#include <osg/Vec4ub>

#include <components/esm/refid.hpp>

#include "terrainweights.hpp"

#include <vector>

namespace Terrain
{
    struct LayerInfo;
    class Storage;

    /// Utility class for subdividing terrain geometry to increase vertex density
    /// Used for snow deformation to create smoother displacement
    class TerrainSubdivider
    {
    public:
        /// Subdivide a geometry by splitting each triangle into 4 smaller triangles recursively
        /// @param source The original geometry to subdivide
        /// @param levels Number of subdivision levels (1=4x triangles, 2=16x, 3=64x)
        /// @return New subdivided geometry, or nullptr on failure
        static osg::ref_ptr<osg::Geometry> subdivide(const osg::Geometry* source, int levels);

        /// Subdivide with terrain weight computation for deformable terrain
        /// @param source The original geometry to subdivide
        /// @param levels Number of subdivision levels
        /// @param chunkCenter Chunk center in cell units
        /// @param chunkSize Chunk size in cell units
        /// @param layerList Texture layers for this chunk
        /// @param blendmaps Blendmap images for this chunk
        /// @param terrainStorage Terrain storage for querying data
        /// @param worldspace Current worldspace
        /// @param playerPosition Player world position (for LOD calculation)
        /// @param cellWorldSize Size of one cell in world units
        /// @param forcedLOD Optional LOD override to force specific quality level (default: LOD_FULL)
        /// @return New subdivided geometry with terrain weights, or nullptr on failure
        static osg::ref_ptr<osg::Geometry> subdivideWithWeights(
            const osg::Geometry* source,
            int levels,
            const osg::Vec2f& chunkCenter,
            float chunkSize,
            const std::vector<LayerInfo>& layerList,
            const std::vector<osg::ref_ptr<osg::Image>>& blendmaps,
            Storage* terrainStorage,
            ESM::RefId worldspace,
            const osg::Vec3f& playerPosition,
            float cellWorldSize,
            TerrainWeights::WeightLOD forcedLOD = TerrainWeights::LOD_FULL);

    private:
        /// Process triangles from a DrawElements primitive set
        static void subdivideTriangles(
            const osg::DrawElements* primitives,
            const osg::Vec3Array* srcVerts,
            const osg::Vec3Array* srcNormals,
            const osg::Vec2Array* srcUVs,
            const osg::Vec4ubArray* srcColors,
            osg::Vec3Array* dstVerts,
            osg::Vec3Array* dstNormals,
            osg::Vec2Array* dstUVs,
            osg::Vec4ubArray* dstColors,
            int levels);

        /// Process triangles with weight computation
        static void subdivideTrianglesWithWeights(
            const osg::DrawElements* primitives,
            const osg::Vec3Array* srcVerts,
            const osg::Vec3Array* srcNormals,
            const osg::Vec2Array* srcUVs,
            const osg::Vec4ubArray* srcColors,
            const osg::Vec4Array* srcWeights,
            osg::Vec3Array* dstVerts,
            osg::Vec3Array* dstNormals,
            osg::Vec2Array* dstUVs,
            osg::Vec4ubArray* dstColors,
            osg::Vec4Array* dstWeights,
            int levels);

        /// Recursively subdivide a single triangle
        static void subdivideTriangleRecursive(
            const osg::Vec3& v0, const osg::Vec3& v1, const osg::Vec3& v2,
            const osg::Vec3& n0, const osg::Vec3& n1, const osg::Vec3& n2,
            const osg::Vec2& uv0, const osg::Vec2& uv1, const osg::Vec2& uv2,
            const osg::Vec4ub& c0, const osg::Vec4ub& c1, const osg::Vec4ub& c2,
            osg::Vec3Array* dstV,
            osg::Vec3Array* dstN,
            osg::Vec2Array* dstUV,
            osg::Vec4ubArray* dstC,
            int level);

        /// Recursively subdivide a triangle with terrain weights
        static void subdivideTriangleRecursiveWithWeights(
            const osg::Vec3& v0, const osg::Vec3& v1, const osg::Vec3& v2,
            const osg::Vec3& n0, const osg::Vec3& n1, const osg::Vec3& n2,
            const osg::Vec2& uv0, const osg::Vec2& uv1, const osg::Vec2& uv2,
            const osg::Vec4ub& c0, const osg::Vec4ub& c1, const osg::Vec4ub& c2,
            const osg::Vec4& w0, const osg::Vec4& w1, const osg::Vec4& w2,
            osg::Vec3Array* dstV,
            osg::Vec3Array* dstN,
            osg::Vec2Array* dstUV,
            osg::Vec4ubArray* dstC,
            osg::Vec4Array* dstW,
            int level);

        /// Interpolate and normalize a normal vector
        static osg::Vec3 interpolateNormal(const osg::Vec3& n0, const osg::Vec3& n1);

        /// Interpolate a color value
        static osg::Vec4ub interpolateColor(const osg::Vec4ub& c0, const osg::Vec4ub& c1);

        /// Interpolate terrain weights
        static osg::Vec4 interpolateWeights(const osg::Vec4& w0, const osg::Vec4& w1);
    };
}

#endif
