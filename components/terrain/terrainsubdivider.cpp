#include "terrainsubdivider.hpp"
#include "terrainweights.hpp"
#include "storage.hpp"
#include "defs.hpp"

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

    osg::Vec4 TerrainSubdivider::interpolateWeights(const osg::Vec4& w0, const osg::Vec4& w1)
    {
        // Delegate to TerrainWeights for normalized interpolation
        return TerrainWeights::interpolateWeights(w0, w1);
    }

    osg::ref_ptr<osg::Geometry> TerrainSubdivider::subdivideWithWeights(
        const osg::Geometry* source,
        int levels,
        const osg::Vec2f& chunkCenter,
        float chunkSize,
        const std::vector<LayerInfo>& layerList,
        const std::vector<osg::ref_ptr<osg::Image>>& blendmaps,
        Storage* terrainStorage,
        ESM::RefId worldspace,
        const osg::Vec3f& playerPosition,
        float cellWorldSize)
    {
        if (!source)
        {
            OSG_WARN << "TerrainSubdivider::subdivideWithWeights: null source geometry" << std::endl;
            return nullptr;
        }

        // Get source arrays
        const osg::Vec3Array* srcVerts = dynamic_cast<const osg::Vec3Array*>(source->getVertexArray());
        const osg::Vec3Array* srcNormals = dynamic_cast<const osg::Vec3Array*>(source->getNormalArray());
        const osg::Vec2Array* srcUVs = dynamic_cast<const osg::Vec2Array*>(source->getTexCoordArray(0));
        const osg::Vec4ubArray* srcColors = dynamic_cast<const osg::Vec4ubArray*>(source->getColorArray());

        if (!srcVerts || !srcNormals || !srcUVs)
        {
            Log(Debug::Warning) << "[TERRAIN] Missing required vertex arrays for subdivision";
            return nullptr;
        }

        // Calculate distance from player to chunk center for LOD determination
        osg::Vec2f chunkWorldCenter = chunkCenter * cellWorldSize;
        osg::Vec2f playerPos2D(playerPosition.x(), playerPosition.y());
        float distanceToPlayer = (chunkWorldCenter - playerPos2D).length();

        // Determine LOD level for weight computation
        TerrainWeights::WeightLOD weightLOD = TerrainWeights::determineLOD(distanceToPlayer);

        // Compute initial terrain weights for source vertices
        osg::ref_ptr<osg::Vec4Array> srcWeights = TerrainWeights::computeWeights(
            srcVerts, chunkCenter, chunkSize, layerList, blendmaps,
            terrainStorage, worldspace, playerPosition, cellWorldSize, weightLOD);

        if (!srcWeights || srcWeights->empty())
        {
            Log(Debug::Warning) << "[TERRAIN] Failed to compute weights for chunk at (" << chunkCenter.x() << ", " << chunkCenter.y() << ")";
            return nullptr;
        }

        // If no subdivision needed, just attach weights and return
        if (levels == 0)
        {
            osg::ref_ptr<osg::Geometry> result = osg::clone(source, osg::CopyOp::DEEP_COPY_ALL);
            result->setVertexAttribArray(6, srcWeights, osg::Array::BIND_PER_VERTEX);
            return result;
        }

        if (levels < 0 || levels > 4)
        {
            Log(Debug::Warning) << "[TERRAIN] Invalid subdivision level " << levels;
            return nullptr;
        }

        // Create destination arrays
        osg::ref_ptr<osg::Vec3Array> dstVerts = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec3Array> dstNormals = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec2Array> dstUVs = new osg::Vec2Array;
        osg::ref_ptr<osg::Vec4ubArray> dstColors = srcColors ? new osg::Vec4ubArray : nullptr;
        osg::ref_ptr<osg::Vec4Array> dstWeights = new osg::Vec4Array;

        // Reserve space (rough estimate: 4^levels times original)
        size_t estimatedVerts = static_cast<size_t>(srcVerts->size() * std::pow(4, levels));
        dstVerts->reserve(estimatedVerts);
        dstNormals->reserve(estimatedVerts);
        dstUVs->reserve(estimatedVerts);
        dstWeights->reserve(estimatedVerts);
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
                    subdivideTrianglesWithWeights(de, srcVerts, srcNormals, srcUVs, srcColors, srcWeights.get(),
                                                 dstVerts.get(), dstNormals.get(), dstUVs.get(),
                                                 dstColors.get(), dstWeights.get(), levels);
                }
                else
                {
                    // Handle DrawArrays
                    const osg::DrawArrays* da = dynamic_cast<const osg::DrawArrays*>(ps);
                    if (da)
                    {
                        for (GLint t = 0; t < da->getCount(); t += 3)
                        {
                            unsigned int i0 = da->getFirst() + t;
                            unsigned int i1 = da->getFirst() + t + 1;
                            unsigned int i2 = da->getFirst() + t + 2;

                            subdivideTriangleRecursiveWithWeights(
                                (*srcVerts)[i0], (*srcVerts)[i1], (*srcVerts)[i2],
                                (*srcNormals)[i0], (*srcNormals)[i1], (*srcNormals)[i2],
                                (*srcUVs)[i0], (*srcUVs)[i1], (*srcUVs)[i2],
                                srcColors ? (*srcColors)[i0] : osg::Vec4ub(255, 255, 255, 255),
                                srcColors ? (*srcColors)[i1] : osg::Vec4ub(255, 255, 255, 255),
                                srcColors ? (*srcColors)[i2] : osg::Vec4ub(255, 255, 255, 255),
                                (*srcWeights)[i0], (*srcWeights)[i1], (*srcWeights)[i2],
                                dstVerts.get(), dstNormals.get(), dstUVs.get(),
                                dstColors.get(), dstWeights.get(), levels);
                        }
                    }
                }
            }
        }

        // Create result geometry
        osg::ref_ptr<osg::Geometry> result = new osg::Geometry;
        result->setVertexArray(dstVerts);
        result->setNormalArray(dstNormals, osg::Array::BIND_PER_VERTEX);
        result->setTexCoordArray(0, dstUVs);
        if (dstColors)
            result->setColorArray(dstColors, osg::Array::BIND_PER_VERTEX);

        // Attach terrain weights as vertex attribute 6
        result->setVertexAttribArray(6, dstWeights, osg::Array::BIND_PER_VERTEX);

        // Create single triangle list primitive
        result->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, dstVerts->size()));

        // Copy state set from source
        if (source->getStateSet())
            result->setStateSet(osg::clone(source->getStateSet(), osg::CopyOp::DEEP_COPY_ALL));

        return result;
    }

    void TerrainSubdivider::subdivideTrianglesWithWeights(
        const osg::DrawElements* primitives,
        const osg::Vec3Array* srcV,
        const osg::Vec3Array* srcN,
        const osg::Vec2Array* srcUV,
        const osg::Vec4ubArray* srcC,
        const osg::Vec4Array* srcW,
        osg::Vec3Array* dstV,
        osg::Vec3Array* dstN,
        osg::Vec2Array* dstUV,
        osg::Vec4ubArray* dstC,
        osg::Vec4Array* dstW,
        int levels)
    {
        for (unsigned int i = 0; i < primitives->getNumIndices(); i += 3)
        {
            unsigned int i0 = primitives->index(i);
            unsigned int i1 = primitives->index(i + 1);
            unsigned int i2 = primitives->index(i + 2);

            subdivideTriangleRecursiveWithWeights(
                (*srcV)[i0], (*srcV)[i1], (*srcV)[i2],
                (*srcN)[i0], (*srcN)[i1], (*srcN)[i2],
                (*srcUV)[i0], (*srcUV)[i1], (*srcUV)[i2],
                srcC ? (*srcC)[i0] : osg::Vec4ub(255, 255, 255, 255),
                srcC ? (*srcC)[i1] : osg::Vec4ub(255, 255, 255, 255),
                srcC ? (*srcC)[i2] : osg::Vec4ub(255, 255, 255, 255),
                (*srcW)[i0], (*srcW)[i1], (*srcW)[i2],
                dstV, dstN, dstUV, dstC, dstW, levels);
        }
    }

    void TerrainSubdivider::subdivideTriangleRecursiveWithWeights(
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

            dstW->push_back(w0);
            dstW->push_back(w1);
            dstW->push_back(w2);

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

        // Interpolate terrain weights
        osg::Vec4 w01 = interpolateWeights(w0, w1);
        osg::Vec4 w12 = interpolateWeights(w1, w2);
        osg::Vec4 w20 = interpolateWeights(w2, w0);

        // Recurse on 4 sub-triangles
        subdivideTriangleRecursiveWithWeights(v0, v01, v20, n0, n01, n20, uv0, uv01, uv20,
                                             c0, c01, c20, w0, w01, w20,
                                             dstV, dstN, dstUV, dstC, dstW, level - 1);

        subdivideTriangleRecursiveWithWeights(v01, v1, v12, n01, n1, n12, uv01, uv1, uv12,
                                             c01, c1, c12, w01, w1, w12,
                                             dstV, dstN, dstUV, dstC, dstW, level - 1);

        subdivideTriangleRecursiveWithWeights(v20, v12, v2, n20, n12, n2, uv20, uv12, uv2,
                                             c20, c12, c2, w20, w12, w2,
                                             dstV, dstN, dstUV, dstC, dstW, level - 1);

        subdivideTriangleRecursiveWithWeights(v01, v12, v20, n01, n12, n20, uv01, uv12, uv20,
                                             c01, c12, c20, w01, w12, w20,
                                             dstV, dstN, dstUV, dstC, dstW, level - 1);
    }
}
