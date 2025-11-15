#include "terrainsubdivider.hpp"

#include <osg/Array>
#include <osg/CopyOp>
#include <osg/PrimitiveSet>
#include <osg/Notify>

#include <cmath>

namespace Terrain
{
    osg::ref_ptr<osg::Geometry> TerrainSubdivider::subdivide(const osg::Geometry* source, int levels)
    {
        if (!source)
        {
            OSG_WARN << "TerrainSubdivider::subdivide: null source geometry" << std::endl;
            return nullptr;
        }

        if (levels == 0)
        {
            // No subdivision, return deep copy
            return osg::clone(source, osg::CopyOp::DEEP_COPY_ALL);
        }

        if (levels < 0 || levels > 4)
        {
            OSG_WARN << "TerrainSubdivider::subdivide: invalid subdivision level " << levels
                     << " (must be 0-4)" << std::endl;
            return nullptr;
        }

        // Get source arrays
        const osg::Vec3Array* srcVerts = dynamic_cast<const osg::Vec3Array*>(source->getVertexArray());
        const osg::Vec3Array* srcNormals = dynamic_cast<const osg::Vec3Array*>(source->getNormalArray());
        const osg::Vec2Array* srcUVs = dynamic_cast<const osg::Vec2Array*>(source->getTexCoordArray(0));
        const osg::Vec4ubArray* srcColors = dynamic_cast<const osg::Vec4ubArray*>(source->getColorArray());

        if (!srcVerts || !srcNormals || !srcUVs)
        {
            OSG_WARN << "TerrainSubdivider::subdivide: missing required arrays (vertices, normals, or UVs)" << std::endl;
            return nullptr;
        }

        // Create destination arrays
        osg::ref_ptr<osg::Vec3Array> dstVerts = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec3Array> dstNormals = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec2Array> dstUVs = new osg::Vec2Array;
        osg::ref_ptr<osg::Vec4ubArray> dstColors = srcColors ? new osg::Vec4ubArray : nullptr;

        // Reserve space (rough estimate: 4^levels times original)
        size_t estimatedVerts = static_cast<size_t>(srcVerts->size() * std::pow(4, levels));
        dstVerts->reserve(estimatedVerts);
        dstNormals->reserve(estimatedVerts);
        dstUVs->reserve(estimatedVerts);
        if (dstColors)
            dstColors->reserve(estimatedVerts);

        // Process each primitive set
        for (unsigned int i = 0; i < source->getNumPrimitiveSets(); ++i)
        {
            const osg::PrimitiveSet* ps = source->getPrimitiveSet(i);

            if (ps->getMode() == GL_TRIANGLES)
            {
                const osg::DrawElements* de = dynamic_cast<const osg::DrawElements*>(ps);
                if (de)
                {
                    subdivideTriangles(de, srcVerts, srcNormals, srcUVs, srcColors,
                                      dstVerts.get(), dstNormals.get(),
                                      dstUVs.get(), dstColors.get(), levels);
                }
                else
                {
                    // Handle DrawArrays
                    const osg::DrawArrays* da = dynamic_cast<const osg::DrawArrays*>(ps);
                    if (da)
                    {
                        // Process triangles directly from arrays
                        for (GLint t = 0; t < da->getCount(); t += 3)
                        {
                            unsigned int i0 = da->getFirst() + t;
                            unsigned int i1 = da->getFirst() + t + 1;
                            unsigned int i2 = da->getFirst() + t + 2;

                            subdivideTriangleRecursive(
                                (*srcVerts)[i0], (*srcVerts)[i1], (*srcVerts)[i2],
                                (*srcNormals)[i0], (*srcNormals)[i1], (*srcNormals)[i2],
                                (*srcUVs)[i0], (*srcUVs)[i1], (*srcUVs)[i2],
                                srcColors ? (*srcColors)[i0] : osg::Vec4ub(255, 255, 255, 255),
                                srcColors ? (*srcColors)[i1] : osg::Vec4ub(255, 255, 255, 255),
                                srcColors ? (*srcColors)[i2] : osg::Vec4ub(255, 255, 255, 255),
                                dstVerts.get(), dstNormals.get(), dstUVs.get(), dstColors.get(),
                                levels);
                        }
                    }
                }
            }
            else
            {
                OSG_WARN << "TerrainSubdivider::subdivide: unsupported primitive mode "
                         << ps->getMode() << " (only GL_TRIANGLES supported)" << std::endl;
            }
        }

        // Create result geometry
        osg::ref_ptr<osg::Geometry> result = new osg::Geometry;
        result->setVertexArray(dstVerts);
        result->setNormalArray(dstNormals, osg::Array::BIND_PER_VERTEX);
        result->setTexCoordArray(0, dstUVs);
        if (dstColors)
            result->setColorArray(dstColors, osg::Array::BIND_PER_VERTEX);

        // Create single triangle list primitive
        result->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, dstVerts->size()));

        // Copy state set from source
        if (source->getStateSet())
            result->setStateSet(osg::clone(source->getStateSet(), osg::CopyOp::DEEP_COPY_ALL));

        OSG_WARN << "[SNOW DEBUG] TerrainSubdivider::subdivide: subdivided " << srcVerts->size()
                 << " verts to " << dstVerts->size() << " verts (level " << levels << ")" << std::endl;

        return result;
    }

    void TerrainSubdivider::subdivideTriangles(
        const osg::DrawElements* primitives,
        const osg::Vec3Array* srcV,
        const osg::Vec3Array* srcN,
        const osg::Vec2Array* srcUV,
        const osg::Vec4ubArray* srcC,
        osg::Vec3Array* dstV,
        osg::Vec3Array* dstN,
        osg::Vec2Array* dstUV,
        osg::Vec4ubArray* dstC,
        int levels)
    {
        // Process triangles in groups of 3 indices
        for (unsigned int i = 0; i < primitives->getNumIndices(); i += 3)
        {
            unsigned int i0 = primitives->index(i);
            unsigned int i1 = primitives->index(i + 1);
            unsigned int i2 = primitives->index(i + 2);

            subdivideTriangleRecursive(
                (*srcV)[i0], (*srcV)[i1], (*srcV)[i2],
                (*srcN)[i0], (*srcN)[i1], (*srcN)[i2],
                (*srcUV)[i0], (*srcUV)[i1], (*srcUV)[i2],
                srcC ? (*srcC)[i0] : osg::Vec4ub(255, 255, 255, 255),
                srcC ? (*srcC)[i1] : osg::Vec4ub(255, 255, 255, 255),
                srcC ? (*srcC)[i2] : osg::Vec4ub(255, 255, 255, 255),
                dstV, dstN, dstUV, dstC, levels);
        }
    }

    void TerrainSubdivider::subdivideTriangleRecursive(
        const osg::Vec3& v0, const osg::Vec3& v1, const osg::Vec3& v2,
        const osg::Vec3& n0, const osg::Vec3& n1, const osg::Vec3& n2,
        const osg::Vec2& uv0, const osg::Vec2& uv1, const osg::Vec2& uv2,
        const osg::Vec4ub& c0, const osg::Vec4ub& c1, const osg::Vec4ub& c2,
        osg::Vec3Array* dstV,
        osg::Vec3Array* dstN,
        osg::Vec2Array* dstUV,
        osg::Vec4ubArray* dstC,
        int level)
    {
        if (level == 0)
        {
            // Base case: add triangle
            dstV->push_back(v0);
            dstV->push_back(v1);
            dstV->push_back(v2);

            dstN->push_back(n0);
            dstN->push_back(n1);
            dstN->push_back(n2);

            dstUV->push_back(uv0);
            dstUV->push_back(uv1);
            dstUV->push_back(uv2);

            if (dstC)
            {
                dstC->push_back(c0);
                dstC->push_back(c1);
                dstC->push_back(c2);
            }
            return;
        }

        // Calculate midpoints
        osg::Vec3 v01 = (v0 + v1) * 0.5f;
        osg::Vec3 v12 = (v1 + v2) * 0.5f;
        osg::Vec3 v20 = (v2 + v0) * 0.5f;

        osg::Vec3 n01 = interpolateNormal(n0, n1);
        osg::Vec3 n12 = interpolateNormal(n1, n2);
        osg::Vec3 n20 = interpolateNormal(n2, n0);

        osg::Vec2 uv01 = (uv0 + uv1) * 0.5f;
        osg::Vec2 uv12 = (uv1 + uv2) * 0.5f;
        osg::Vec2 uv20 = (uv2 + uv0) * 0.5f;

        osg::Vec4ub c01 = interpolateColor(c0, c1);
        osg::Vec4ub c12 = interpolateColor(c1, c2);
        osg::Vec4ub c20 = interpolateColor(c2, c0);

        // Recurse on 4 sub-triangles
        //      v0
        //      /\
        //   v01/__\v20
        //    /\  /\
        // v1/__\/___\v2
        //      v12

        subdivideTriangleRecursive(v0, v01, v20, n0, n01, n20, uv0, uv01, uv20, c0, c01, c20,
                                   dstV, dstN, dstUV, dstC, level - 1);

        subdivideTriangleRecursive(v01, v1, v12, n01, n1, n12, uv01, uv1, uv12, c01, c1, c12,
                                   dstV, dstN, dstUV, dstC, level - 1);

        subdivideTriangleRecursive(v20, v12, v2, n20, n12, n2, uv20, uv12, uv2, c20, c12, c2,
                                   dstV, dstN, dstUV, dstC, level - 1);

        subdivideTriangleRecursive(v01, v12, v20, n01, n12, n20, uv01, uv12, uv20, c01, c12, c20,
                                   dstV, dstN, dstUV, dstC, level - 1);
    }

    osg::Vec3 TerrainSubdivider::interpolateNormal(const osg::Vec3& n0, const osg::Vec3& n1)
    {
        osg::Vec3 result = (n0 + n1);
        result.normalize();
        return result;
    }

    osg::Vec4ub TerrainSubdivider::interpolateColor(const osg::Vec4ub& c0, const osg::Vec4ub& c1)
    {
        return osg::Vec4ub(
            (c0.r() + c1.r()) / 2,
            (c0.g() + c1.g()) / 2,
            (c0.b() + c1.b()) / 2,
            (c0.a() + c1.a()) / 2);
    }
}
