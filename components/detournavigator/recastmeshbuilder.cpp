#include "recastmeshbuilder.hpp"
#include "exceptions.hpp"
#include "heightfieldmeshbuilder.hpp"
#include "recastmeshobject.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/convert.hpp>
#include <components/physicshelpers/heightfield.hpp>
#include <components/physicshelpers/transformboundingbox.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Real.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cxxabi.h>
#include <sstream>
#include <vector>

namespace DetourNavigator
{
    namespace
    {
        JPH::RVec3 joltTransformMult(const JPH::Float3& vec, const osg::Matrixd& mat)
        {
            osg::Vec3f pos(vec.x, vec.y, vec.z);
            osg::Vec3d result = pos * mat;
            return JPH::RVec3(result.x(), result.y(), result.z());
        }

        void walkShapeTriangles(const JPH::Shape& shape, const JPH::AABox& bounds, TriangleWalkerFunc& walkerFunc,
            JPH::Vec3 localScale = JPH::Vec3::sReplicate(1.0f), JPH::Quat rotation = JPH::Quat::sIdentity())
        {
            // Start iterating all triangles of the shape
            JPH::Shape::GetTrianglesContext context;
            shape.GetTrianglesStart(context, bounds, JPH::Vec3::sZero(), rotation, localScale);

            int triangleIndex = 0;
            constexpr int cMaxTrianglesInBatch = 256;
            JPH::Float3 vertices[3 * cMaxTrianglesInBatch];
            for (;;)
            {
                // Get the next batch of triangles and vertices
                int triangle_count = shape.GetTrianglesNext(context, cMaxTrianglesInBatch, vertices);
                assert(triangle_count >= 0);
                if (triangle_count == 0)
                    break;

                for (int vertex = 0, vMax = 3 * triangle_count; vertex < vMax; vertex += 3, ++triangleIndex)
                    walkerFunc(vertices[vertex + 0], vertices[vertex + 1], vertices[vertex + 2], triangleIndex);
            }
        }

        inline bool TestTriangleAgainstAabb2(
            const JPH::RVec3* vertices, const JPH::RVec3& aabbMin, const JPH::RVec3& aabbMax)
        {
            const JPH::RVec3& p1 = vertices[0];
            const JPH::RVec3& p2 = vertices[1];
            const JPH::RVec3& p3 = vertices[2];

            if (std::min(std::min(p1.GetX(), p2.GetX()), p3.GetX()) > aabbMax.GetX())
                return false;
            if (std::max(std::max(p1.GetX(), p2.GetX()), p3.GetX()) < aabbMin.GetX())
                return false;

            if (std::min(std::min(p1.GetZ(), p2.GetZ()), p3.GetZ()) > aabbMax.GetZ())
                return false;
            if (std::max(std::max(p1.GetZ(), p2.GetZ()), p3.GetZ()) < aabbMin.GetZ())
                return false;

            if (std::min(std::min(p1.GetY(), p2.GetY()), p3.GetY()) > aabbMax.GetY())
                return false;
            if (std::max(std::max(p1.GetY(), p2.GetY()), p3.GetY()) < aabbMin.GetY())
                return false;
            return true;
        }

        RecastMeshTriangle makeRecastMeshTriangle(const JPH::RVec3* vertices, const AreaType areaType)
        {
            RecastMeshTriangle result;
            result.mAreaType = areaType;
            for (std::size_t i = 0; i < 3; ++i)
                result.mVertices[i] = Misc::Convert::toOsg(vertices[i]);
            return result;
        }

        float getHeightfieldScale(int cellSize, std::size_t dataSize)
        {
            return static_cast<float>(cellSize) / (dataSize - 1);
        }

        bool isNan(const RecastMeshTriangle& triangle)
        {
            for (std::size_t i = 0; i < 3; ++i)
                if (std::isnan(triangle.mVertices[i].x()) || std::isnan(triangle.mVertices[i].y())
                    || std::isnan(triangle.mVertices[i].z()))
                    return true;
            return false;
        }
    }

    Mesh makeMesh(std::vector<RecastMeshTriangle>&& triangles, const osg::Vec3f& shift)
    {
        std::vector<osg::Vec3f> uniqueVertices;
        uniqueVertices.reserve(3 * triangles.size());

        for (const RecastMeshTriangle& triangle : triangles)
            for (const osg::Vec3f& vertex : triangle.mVertices)
                uniqueVertices.push_back(vertex);

        std::sort(uniqueVertices.begin(), uniqueVertices.end());
        uniqueVertices.erase(std::unique(uniqueVertices.begin(), uniqueVertices.end()), uniqueVertices.end());

        std::vector<int> indices;
        indices.reserve(3 * triangles.size());
        std::vector<AreaType> areaTypes;
        areaTypes.reserve(triangles.size());

        for (const RecastMeshTriangle& triangle : triangles)
        {
            areaTypes.push_back(triangle.mAreaType);

            for (const osg::Vec3f& vertex : triangle.mVertices)
            {
                const auto it = std::lower_bound(uniqueVertices.begin(), uniqueVertices.end(), vertex);
                assert(it != uniqueVertices.end());
                assert(*it == vertex);
                indices.push_back(static_cast<int>(it - uniqueVertices.begin()));
            }
        }

        triangles.clear();

        std::vector<float> vertices;
        vertices.reserve(3 * uniqueVertices.size());

        for (const osg::Vec3f& vertex : uniqueVertices)
        {
            vertices.push_back(vertex.x() + shift.x());
            vertices.push_back(vertex.y() + shift.y());
            vertices.push_back(vertex.z() + shift.z());
        }

        return Mesh(std::move(indices), std::move(vertices), std::move(areaTypes));
    }

    Mesh makeMesh(const Heightfield& heightfield)
    {
        using Misc::Convert::toOsg;

        HeightfieldMeshBuilder shape(static_cast<int>(heightfield.mHeights.size() / heightfield.mLength),
            static_cast<int>(heightfield.mLength), heightfield.mHeights.data(), heightfield.mMinHeight,
            heightfield.mMaxHeight);

        const float scale = getHeightfieldScale(heightfield.mCellSize, heightfield.mOriginalSize);
        shape.setLocalScaling(JPH::Vec3(scale, scale, 1));
        JPH::Vec3 aabbMin;
        JPH::Vec3 aabbMax;
        shape.getAabb(aabbMin, aabbMax);
        std::vector<RecastMeshTriangle> triangles;

        TriangleProcessFunc callback = [&](JPH::RVec3* vertices, int, int) {
            triangles.emplace_back(makeRecastMeshTriangle(vertices, AreaType_ground));
        };
        shape.processAllTriangles(callback, aabbMin, aabbMax);

        const osg::Vec2f aabbShift
            = (osg::Vec2f(aabbMax.GetX(), aabbMax.GetY()) - osg::Vec2f(aabbMin.GetX(), aabbMin.GetY())) * 0.5;
        const osg::Vec2f tileShift = osg::Vec2f(heightfield.mMinX, heightfield.mMinY) * scale;
        const osg::Vec2f localShift = aabbShift + tileShift;
        const float cellSize = static_cast<float>(heightfield.mCellSize);
        const osg::Vec3f cellShift(heightfield.mCellPosition.x() * cellSize, heightfield.mCellPosition.y() * cellSize,
            (heightfield.mMinHeight + heightfield.mMaxHeight) * 0.5f);
        return makeMesh(std::move(triangles), cellShift + osg::Vec3f(localShift.x(), localShift.y(), 0));
    }

    RecastMeshBuilder::RecastMeshBuilder(const TileBounds& bounds) noexcept
        : mBounds(bounds)
    {
    }

    void RecastMeshBuilder::addObject(const JPH::Shape& shape, const osg::Matrixd& transform, const AreaType areaType,
        osg::ref_ptr<const Resource::PhysicsShape> source, const ObjectTransform& objectTransform)
    {
        addObject(shape, transform, areaType);
        mSources.push_back(MeshSource{ std::move(source), objectTransform, areaType });
    }

    void RecastMeshBuilder::addObject(const JPH::Shape& shape, const osg::Matrixd& transform, const AreaType areaType)
    {
        if (dynamic_cast<const JPH::CompoundShape*>(&shape))
            return addObject(static_cast<const JPH::CompoundShape&>(shape), transform, areaType);

        if (dynamic_cast<const JPH::HeightFieldShape*>(&shape))
            return addObject(static_cast<const JPH::HeightFieldShape&>(shape), transform, areaType);

        if (dynamic_cast<const JPH::MeshShape*>(&shape))
            return addObject(static_cast<const JPH::MeshShape&>(shape), transform, areaType);

        if (dynamic_cast<const JPH::BoxShape*>(&shape))
            return addObject(static_cast<const JPH::BoxShape&>(shape), transform, areaType);

        // TODO: apply scale to transform?
        if (dynamic_cast<const JPH::ScaledShape*>(&shape))
            return addObject(*static_cast<const JPH::ScaledShape&>(shape).GetInnerShape(), transform, areaType);

        std::ostringstream message;
        message << "Unsupported shape type: " << typeid(shape).name();
        throw InvalidArgument(message.str());
    }

    void RecastMeshBuilder::addObject(
        const JPH::CompoundShape& shape, const osg::Matrixd& transform, const AreaType areaType)
    {
        for (int i = 0, num = shape.GetNumSubShapes(); i < num; ++i)
        {
            auto childTransform = getSubShapeTransform(shape.GetSubShape(i));
            addObject(*shape.GetSubShape(i).mShape.GetPtr(), childTransform * transform, areaType);
        }
    }

    void RecastMeshBuilder::addObject(
        const JPH::MeshShape& shape, const osg::Matrixd& transform, const AreaType areaType)
    {
        // FIXME: we can optimize this by not having a callback for each triangle
        // but instead process in batches when reading from the jolt shape
        TriangleProcessFunc callback = [&](JPH::RVec3* vertices, int, int) {
            RecastMeshTriangle triangle = makeRecastMeshTriangle(vertices, areaType);
            std::reverse(triangle.mVertices.begin(), triangle.mVertices.end());
            mTriangles.emplace_back(triangle);
        };
        return addObject(shape, transform, callback);
    }

    void RecastMeshBuilder::addObject(
        const JPH::HeightFieldShape& shape, const osg::Matrixd& transform, const AreaType areaType)
    {
        // FIXME: we can optimize this by not having a callback for each triangle
        // but instead process in batches when reading from the jolt shape
        TriangleProcessFunc callback = [&](JPH::RVec3* vertices, int, int) {
            mTriangles.emplace_back(makeRecastMeshTriangle(vertices, areaType));
        };
        addObject(shape, transform, callback);
    }

    void RecastMeshBuilder::addObject(
        const JPH::BoxShape& shape, const osg::Matrixd& transform, const AreaType areaType)
    {
        TriangleWalkerFunc walkerFunc = [&](JPH::Float3& v1, JPH::Float3& v2, JPH::Float3& v3, int) {
            // Convert to a world space triangle set
            std::array<JPH::RVec3, 3> transformed;
            transformed[0] = joltTransformMult(v1, transform);
            transformed[1] = joltTransformMult(v2, transform);
            transformed[2] = joltTransformMult(v3, transform);

            mTriangles.emplace_back(makeRecastMeshTriangle(transformed.data(), areaType));
        };
        walkShapeTriangles(shape, JPH::AABox::sBiggest(), walkerFunc);
    }

    void RecastMeshBuilder::addWater(const osg::Vec2i& cellPosition, const Water& water)
    {
        mWater.push_back(CellWater{ cellPosition, water });
    }

    void RecastMeshBuilder::addHeightfield(const osg::Vec2i& cellPosition, int cellSize, float height)
    {
        if (const auto intersection = getIntersection(mBounds, maxCellTileBounds(cellPosition, cellSize)))
            mFlatHeightfields.emplace_back(FlatHeightfield{ cellPosition, cellSize, height });
    }

    void RecastMeshBuilder::addHeightfield(const osg::Vec2i& cellPosition, int cellSize, const float* heights,
        std::size_t size, float minHeight, float maxHeight)
    {
        const auto intersection = getIntersection(mBounds, maxCellTileBounds(cellPosition, cellSize));
        if (!intersection.has_value())
            return;

        const osg::Vec3f shift = PhysicsSystemHelpers::getHeightfieldShift(
            cellPosition.x(), cellPosition.y(), cellSize, minHeight, maxHeight);
        const float stepSize = getHeightfieldScale(cellSize, size);
        const int halfCellSize = cellSize / 2;
        const auto local = [&](float v, float shift) { return (v - shift + halfCellSize) / stepSize; };
        const auto index = [&](float v, int add) { return std::clamp<int>(static_cast<int>(v) + add, 0, size); };
        const std::size_t minX = index(std::round(local(intersection->mMin.x(), shift.x())), -1);
        const std::size_t minY = index(std::round(local(intersection->mMin.y(), shift.y())), -1);
        const std::size_t maxX = index(std::round(local(intersection->mMax.x(), shift.x())), 1);
        const std::size_t maxY = index(std::round(local(intersection->mMax.y(), shift.y())), 1);
        const std::size_t endX = std::min(maxX + 1, size);
        const std::size_t endY = std::min(maxY + 1, size);
        const std::size_t sliceSize = (endX - minX) * (endY - minY);
        if (sliceSize == 0)
            return;
        std::vector<float> tileHeights;
        tileHeights.reserve(sliceSize);
        for (std::size_t y = minY; y < endY; ++y)
            for (std::size_t x = minX; x < endX; ++x)
                tileHeights.push_back(heights[x + y * size]);
        Heightfield heightfield;
        heightfield.mCellPosition = cellPosition;
        heightfield.mCellSize = cellSize;
        heightfield.mLength = static_cast<std::uint8_t>(endY - minY);
        heightfield.mMinHeight = minHeight;
        heightfield.mMaxHeight = maxHeight;
        heightfield.mHeights = std::move(tileHeights);
        heightfield.mOriginalSize = size;
        heightfield.mMinX = static_cast<std::uint8_t>(minX);
        heightfield.mMinY = static_cast<std::uint8_t>(minY);
        mHeightfields.push_back(std::move(heightfield));
    }

    std::shared_ptr<RecastMesh> RecastMeshBuilder::create(const Version& version) &&
    {
        mTriangles.erase(std::remove_if(mTriangles.begin(), mTriangles.end(), isNan), mTriangles.end());
        std::sort(mTriangles.begin(), mTriangles.end());
        std::sort(mWater.begin(), mWater.end());
        std::sort(mHeightfields.begin(), mHeightfields.end());
        std::sort(mFlatHeightfields.begin(), mFlatHeightfields.end());
        Mesh mesh = makeMesh(std::move(mTriangles));
        return std::make_shared<RecastMesh>(version, std::move(mesh), std::move(mWater), std::move(mHeightfields),
            std::move(mFlatHeightfields), std::move(mSources));
    }

    void RecastMeshBuilder::addObject(
        const JPH::MeshShape& shape, const osg::Matrixd& transform, TriangleProcessFunc& processTriangle)
    {
        JPH::AABox bounds = shape.GetLocalBounds();

        const JPH::RVec3 boundsMin(mBounds.mMin.x(), mBounds.mMin.y(),
            -std::numeric_limits<double>::max() * std::numeric_limits<double>::epsilon());
        const JPH::RVec3 boundsMax(mBounds.mMax.x(), mBounds.mMax.y(),
            std::numeric_limits<double>::max() * std::numeric_limits<double>::epsilon());

        // Convert to a world space triangle set
        TriangleWalkerFunc walkerFunc = [&](JPH::Float3& v1, JPH::Float3& v2, JPH::Float3& v3, int triangleIndex) {
            std::array<JPH::RVec3, 3> transformed;
            transformed[0] = joltTransformMult(v1, transform);
            transformed[1] = joltTransformMult(v2, transform);
            transformed[2] = joltTransformMult(v3, transform);

            if (TestTriangleAgainstAabb2(transformed.data(), boundsMin, boundsMax))
                processTriangle(transformed.data(), 0, triangleIndex);
        };
        walkShapeTriangles(shape, bounds, walkerFunc);
    }

    void RecastMeshBuilder::addObject(
        const JPH::HeightFieldShape& shape, const osg::Matrixd& transform, TriangleProcessFunc& processTriangle)
    {
        using PhysicsSystemHelpers::transformBoundingBox;

        JPH::AABox bounds = shape.GetLocalBounds();

        auto joltTransform = Misc::Convert::toJoltNoScale(transform);

        transformBoundingBox(joltTransform, bounds.mMin, bounds.mMax);

        bounds.mMin.SetX(std::max(static_cast<float>(mBounds.mMin.x()), bounds.mMin.GetX()));
        bounds.mMin.SetX(std::min(static_cast<float>(mBounds.mMax.x()), bounds.mMin.GetX()));
        bounds.mMin.SetY(std::max(static_cast<float>(mBounds.mMin.y()), bounds.mMin.GetY()));
        bounds.mMin.SetY(std::min(static_cast<float>(mBounds.mMax.y()), bounds.mMin.GetY()));

        bounds.mMax.SetX(std::max(static_cast<float>(mBounds.mMin.x()), bounds.mMax.GetX()));
        bounds.mMax.SetX(std::min(static_cast<float>(mBounds.mMax.x()), bounds.mMax.GetX()));
        bounds.mMax.SetY(std::max(static_cast<float>(mBounds.mMin.y()), bounds.mMax.GetY()));
        bounds.mMax.SetY(std::min(static_cast<float>(mBounds.mMax.y()), bounds.mMax.GetY()));

        JPH::RMat44 inverseMatrix = joltTransform.Inversed();
        transformBoundingBox(inverseMatrix, bounds.mMin, bounds.mMax);

        TriangleWalkerFunc walkerFunc = [&](JPH::Float3& v1, JPH::Float3& v2, JPH::Float3& v3, int triangleIndex) {
            // Convert to a world space triangle set
            std::array<JPH::RVec3, 3> transformed;
            transformed[0] = joltTransformMult(v1, transform);
            transformed[1] = joltTransformMult(v2, transform);
            transformed[2] = joltTransformMult(v3, transform);
            processTriangle(transformed.data(), 0, triangleIndex);
        };
        walkShapeTriangles(shape, bounds, walkerFunc);
    }
}
