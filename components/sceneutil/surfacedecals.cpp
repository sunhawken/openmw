#include "surfacedecals.hpp"

#include <osg/BufferObject>
#include <osgUtil/CullVisitor>

#include <cmath>
#include <utility>

namespace SceneUtil
{
    namespace
    {
        bool finite(const osg::Vec3f& value)
        {
            return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
        }
    }

    SurfaceDecals::SurfaceDecals(std::size_t maxPatches, std::size_t maxVerticesPerPatch)
        : mMaxPatches(maxPatches)
        , mMaxVerticesPerPatch(maxVerticesPerPatch)
    {
        mPatches.reserve(maxPatches);
    }

    bool SurfaceDecals::addPatch(
        const std::vector<Anchor>& vertices, osg::StateSet* state, osg::Node::NodeMask mask)
    {
        if (!mMaxPatches || !state || !mask || vertices.empty() || vertices.size() % 3 != 0
            || vertices.size() > mMaxVerticesPerPatch)
            return false;
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            const Anchor& anchor = vertices[i];
            const float sum = anchor.weights.x() + anchor.weights.y() + anchor.weights.z();
            if (!finite(anchor.weights) || !std::isfinite(anchor.uv.x()) || !std::isfinite(anchor.uv.y())
                || std::abs(sum - 1.f) > 1e-3f || anchor.weights.x() < -1e-4f
                || anchor.weights.y() < -1e-4f || anchor.weights.z() < -1e-4f
                || anchor.indices != vertices[i - i % 3].indices)
                return false;
        }

        Patch patch;
        patch.data = { vertices, state, mask };
        for (Anchor& anchor : patch.data.vertices)
            anchor.weights /= anchor.weights.x() + anchor.weights.y() + anchor.weights.z();
        osg::ref_ptr<osg::Vec2Array> uv = new osg::Vec2Array;
        uv->reserve(vertices.size());
        for (const Anchor& anchor : vertices)
            uv->push_back(anchor.uv);
        uv->setVertexBufferObject(new osg::VertexBufferObject);
        osg::ref_ptr<osg::Vec4Array> color = new osg::Vec4Array;
        color->push_back(osg::Vec4f(1.f, 1.f, 1.f, 1.f));
        for (auto& geometry : patch.geometry)
        {
            geometry = new osg::Geometry;
            geometry->setName("Surface decal");
            geometry->setNodeMask(mask);
            geometry->setStateSet(state);
            geometry->setUseDisplayList(false);
            geometry->setUseVertexBufferObjects(true);
            // The source drawable supplies the conservative animated bound.
            geometry->setCullingActive(false);
            osg::ref_ptr<osg::VertexBufferObject> vbo = new osg::VertexBufferObject;
            vbo->setUsage(GL_DYNAMIC_DRAW_ARB);
            osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array(vertices.size());
            osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array(vertices.size());
            positions->setVertexBufferObject(vbo);
            normals->setVertexBufferObject(vbo);
            geometry->setVertexArray(positions);
            geometry->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
            geometry->setTexCoordArray(0, uv, osg::Array::BIND_PER_VERTEX);
            geometry->setColorArray(color, osg::Array::BIND_OVERALL);
            geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, vertices.size()));
        }
        // Prepare both buffers before changing the visible patch collection.
        if (mPatches.size() == mMaxPatches)
        {
            mPatches.erase(mPatches.begin());
            ++mEvicted;
        }
        mPatches.push_back(std::move(patch));
        return true;
    }

    void SurfaceDecals::clear()
    {
        mPatches.clear();
    }

    std::size_t SurfaceDecals::vertexCount() const
    {
        std::size_t result = 0;
        for (const Patch& patch : mPatches)
            result += patch.data.vertices.size();
        return result;
    }

    bool SurfaceDecals::evaluate(const osg::Geometry& current, const PatchData& patch,
        osg::Vec3Array& positions, osg::Vec3Array& normals)
    {
        const auto* sourcePositions = dynamic_cast<const osg::Vec3Array*>(current.getVertexArray());
        const auto* sourceNormals = dynamic_cast<const osg::Vec3Array*>(current.getNormalArray());
        if (!sourcePositions || positions.size() != patch.vertices.size() || normals.size() != positions.size())
            return false;
        const bool perVertexNormals = sourceNormals && sourceNormals->size() == sourcePositions->size()
            && current.getNormalBinding() == osg::Geometry::BIND_PER_VERTEX;
        const bool overallNormal = sourceNormals && !sourceNormals->empty()
            && current.getNormalBinding() == osg::Geometry::BIND_OVERALL;
        for (std::size_t i = 0; i < patch.vertices.size(); ++i)
        {
            const Anchor& anchor = patch.vertices[i];
            const auto& index = anchor.indices;
            if (index[0] >= sourcePositions->size() || index[1] >= sourcePositions->size()
                || index[2] >= sourcePositions->size())
                return false;
            const osg::Vec3f& a = (*sourcePositions)[index[0]];
            const osg::Vec3f& b = (*sourcePositions)[index[1]];
            const osg::Vec3f& c = (*sourcePositions)[index[2]];
            // The affine form avoids multiplying the whole world/local origin
            // by a slightly rounded barycentric sum.
            positions[i] = a + (b - a) * anchor.weights.y() + (c - a) * anchor.weights.z();
            osg::Vec3f normal;
            if (perVertexNormals)
                normal = (*sourceNormals)[index[0]] * anchor.weights.x()
                    + (*sourceNormals)[index[1]] * anchor.weights.y()
                    + (*sourceNormals)[index[2]] * anchor.weights.z();
            else if (overallNormal)
                normal = sourceNormals->front();
            if (!finite(normal) || normal.length2() < 1e-12f)
            {
                normal = (b - a) ^ (c - a);
                normal.normalize();
            }
            if (!finite(positions[i]) || !finite(normal))
                return false;
            // Preserve interpolated source normals. Normalizing each newly
            // clipped vertex would change per-pixel lighting across the patch.
            normals[i] = normal;
        }
        return true;
    }

    void SurfaceDecals::cull(const osg::Geometry& current, osgUtil::CullVisitor& visitor)
    {
        const unsigned int traversal = visitor.getTraversalNumber();
        // apply(source) has already popped the source's own StateSet; reproduce
        // it here beneath the patch's explicitly supplied material overrides.
        const osg::StateSet* sourceState = current.getStateSet();
        if (sourceState)
            visitor.pushStateSet(sourceState);
        for (Patch& patch : mPatches)
        {
            osg::Geometry& geometry = *patch.geometry[traversal % 2];
            if (!visitor.validNodeMask(geometry))
                continue;
            if (!patch.initialized || patch.lastTraversal != traversal)
            {
                auto* positions = static_cast<osg::Vec3Array*>(geometry.getVertexArray());
                auto* normals = static_cast<osg::Vec3Array*>(geometry.getNormalArray());
                patch.valid = evaluate(current, patch.data, *positions, *normals);
                patch.initialized = true;
                patch.lastTraversal = traversal;
                if (patch.valid)
                {
                    positions->dirty();
                    normals->dirty();
                    geometry.dirtyBound();
                }
            }
            if (patch.valid)
                geometry.accept(visitor);
        }
        if (sourceState)
            visitor.popStateSet();
    }
}
