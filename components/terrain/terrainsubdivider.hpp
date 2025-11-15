#ifndef COMPONENTS_TERRAIN_TERRAINSUBDIVIDER_H
#define COMPONENTS_TERRAIN_TERRAINSUBDIVIDER_H

#include <osg/Geometry>
#include <osg/Vec2>
#include <osg/Vec3>
#include <osg/Vec4ub>

namespace Terrain
{
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

        /// Interpolate and normalize a normal vector
        static osg::Vec3 interpolateNormal(const osg::Vec3& n0, const osg::Vec3& n1);

        /// Interpolate a color value
        static osg::Vec4ub interpolateColor(const osg::Vec4ub& c0, const osg::Vec4ub& c1);
    };
}

#endif
