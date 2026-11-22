#ifndef OPENMW_COMPONENTS_SCENEUTIL_HEMCOVERAGE_HPP
#define OPENMW_COMPONENTS_SCENEUTIL_HEMCOVERAGE_HPP

#include <osg/Geometry>
#include <osg/Vec3d>
#include <osg/Vec4d>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace SceneUtil
{
    // A garment coverage test only: it never moves garment/body vertices or
    // changes simulation. The caller clips the proximal part of a knee with
    // worldPlane, keeping dot(plane.xyz, worldPosition) + plane.w >= 0.
    class HemCoverage
    {
    public:
        struct Evaluation
        {
            bool valid = false;
            bool centerInside = false;
            osg::Vec4d worldPlane;
            double footprintGain = 0;
            double appliedGain = 0;
            double radius = 0;
            double centerDistance = 0; // signed distance to projected hem edges
            double hemClearance = 0; // centerDistance - enclosing knee radius
            double cutDistance = 0; // signed from the knee vertex centroid
            double fullCutDistance = 0;
            double visibleFraction = 1; // fraction of original knee vertices kept
            double visibleAxialFraction = 1;
        };

        static constexpr double Margin = .04;
        static constexpr double FootprintBand = 1.;
        static constexpr double SourceWeldTolerance = 1e-4;

        // The sign is intentionally unspecified. Orient the result toward the
        // distal calf/foot direction once, then transport it with its source
        // geometry's transform. Repeated PCA on a deforming mesh is unnecessary.
        static std::optional<osg::Vec3d> principalAxis(const std::vector<osg::Vec3d>& points)
        {
            if (points.size() < 3)
                return std::nullopt;
            osg::Vec3d mean;
            for (const auto& p : points)
            {
                if (!finite(p))
                    return std::nullopt;
                mean += p / static_cast<double>(points.size());
            }
            double a[3][3]{};
            double vectors[3][3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            for (const auto& p : points)
            {
                const auto d = p - mean;
                for (unsigned i = 0; i < 3; ++i)
                    for (unsigned j = 0; j < 3; ++j)
                        a[i][j] += d[i] * d[j] / static_cast<double>(points.size());
            }
            for (unsigned iteration = 0; iteration < 32; ++iteration)
            {
                unsigned p = 0, q = 1;
                for (unsigned i = 0; i < 3; ++i)
                    for (unsigned j = i + 1; j < 3; ++j)
                        if (std::abs(a[i][j]) > std::abs(a[p][q]))
                        {
                            p = i;
                            q = j;
                        }
                const double scale = std::max({std::abs(a[0][0]), std::abs(a[1][1]), std::abs(a[2][2]), 1.});
                if (std::abs(a[p][q]) <= 1e-14 * scale)
                    break;
                const double angle = .5 * std::atan2(2 * a[p][q], a[q][q] - a[p][p]);
                const double c = std::cos(angle), s = std::sin(angle);
                const double app = a[p][p], aqq = a[q][q], apq = a[p][q];
                a[p][p] = c * c * app - 2 * s * c * apq + s * s * aqq;
                a[q][q] = s * s * app + 2 * s * c * apq + c * c * aqq;
                a[p][q] = a[q][p] = 0;
                for (unsigned k = 0; k < 3; ++k)
                {
                    if (k != p && k != q)
                    {
                        const double akp = a[k][p], akq = a[k][q];
                        a[k][p] = a[p][k] = c * akp - s * akq;
                        a[k][q] = a[q][k] = s * akp + c * akq;
                    }
                    const double vkp = vectors[k][p], vkq = vectors[k][q];
                    vectors[k][p] = c * vkp - s * vkq;
                    vectors[k][q] = s * vkp + c * vkq;
                }
            }
            unsigned best = 0, second = 1;
            if (a[second][second] > a[best][best])
                std::swap(best, second);
            for (unsigned i = 2; i < 3; ++i)
                if (a[i][i] > a[best][best])
                {
                    second = best;
                    best = i;
                }
                else if (a[i][i] > a[second][second])
                    second = i;
            if (!(a[best][best] > 1e-12) || a[best][best] - a[second][second] <= a[best][best] * 1e-6)
                return std::nullopt;
            osg::Vec3d result(vectors[0][best], vectors[1][best], vectors[2][best]);
            if (!finite(result) || result.normalize() <= 1e-12)
                return std::nullopt;
            return result;
        }

        static std::unique_ptr<HemCoverage> create(
            const osg::Geometry& geometry, const std::vector<unsigned>& pureWaistOriginalIndices)
        {
            const auto* floatVertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            const auto* doubleVertices = dynamic_cast<const osg::Vec3dArray*>(geometry.getVertexArray());
            const std::size_t count = floatVertices ? floatVertices->size() : doubleVertices ? doubleVertices->size() : 0;
            if (count < 3 || count > 262144 || pureWaistOriginalIndices.empty())
                return {};
            std::vector<osg::Vec3d> vertices;
            vertices.reserve(count);
            osg::Vec3d mean;
            for (std::size_t i = 0; i < count; ++i)
            {
                const osg::Vec3d p = floatVertices ? osg::Vec3d((*floatVertices)[i]) : (*doubleVertices)[i];
                if (!finite(p))
                    return {};
                vertices.push_back(p);
                mean += p / static_cast<double>(count);
            }
            const auto principal = principalAxis(vertices);
            if (!principal)
                return {};
            osg::Vec3d axis = *principal, waist;
            std::set<unsigned> uniqueWaist(pureWaistOriginalIndices.begin(), pureWaistOriginalIndices.end());
            for (unsigned index : uniqueWaist)
            {
                if (index >= count)
                    return {};
                waist += vertices[index] / static_cast<double>(uniqueWaist.size());
            }
            const double direction = (waist - mean) * axis;
            if (std::abs(direction) <= 1e-8)
                return {};
            if (direction < 0)
                axis = -axis;
            double low = std::numeric_limits<double>::infinity(), high = -low;
            for (const auto& v : vertices)
            {
                low = std::min(low, v * axis);
                high = std::max(high, v * axis);
            }
            if (!(high - low > 1e-6))
                return {};

            std::vector<std::array<unsigned, 3>> triangles;
            for (unsigned p = 0; p < geometry.getNumPrimitiveSets(); ++p)
            {
                const auto* primitive = geometry.getPrimitiveSet(p);
                const unsigned n = primitive->getNumIndices();
                const auto add = [&](unsigned a, unsigned b, unsigned c) {
                    if (a >= count || b >= count || c >= count)
                        return false;
                    if (a != b && b != c && a != c)
                        triangles.push_back({a, b, c});
                    return true;
                };
                if (primitive->getMode() == GL_TRIANGLES)
                {
                    if (n % 3 != 0)
                        return {};
                    for (unsigned i = 0; i < n; i += 3)
                        if (!add(primitive->index(i), primitive->index(i + 1), primitive->index(i + 2)))
                            return {};
                }
                else if (primitive->getMode() == GL_TRIANGLE_STRIP)
                {
                    for (unsigned i = 2; i < n; ++i)
                        if (!add(primitive->index(i - 2 + (i & 1)), primitive->index(i - 1 - (i & 1)), primitive->index(i)))
                            return {};
                }
                else if (primitive->getMode() == GL_TRIANGLE_FAN)
                {
                    for (unsigned i = 2; i < n; ++i)
                        if (!add(primitive->index(0), primitive->index(i - 1), primitive->index(i)))
                            return {};
                }
                else
                    return {};
                if (triangles.size() > 524288)
                    return {};
            }
            if (triangles.empty())
                return {};

            using Cell = std::array<std::int64_t, 3>;
            std::map<Cell, std::vector<unsigned>> cells;
            std::vector<unsigned> groups(count);
            std::vector<std::vector<unsigned>> aliases(count);
            for (unsigned i = 0; i < count; ++i)
            {
                Cell cell;
                for (unsigned j = 0; j < 3; ++j)
                    cell[j] = static_cast<std::int64_t>(std::floor(vertices[i][j] / SourceWeldTolerance));
                unsigned representative = i;
                for (int x = -1; x <= 1; ++x)
                    for (int y = -1; y <= 1; ++y)
                        for (int z = -1; z <= 1; ++z)
                        {
                            const auto found = cells.find({cell[0] + x, cell[1] + y, cell[2] + z});
                            if (found == cells.end())
                                continue;
                            for (unsigned candidate : found->second)
                                if ((vertices[candidate] - vertices[i]).length2() < SourceWeldTolerance * SourceWeldTolerance)
                                    representative = std::min(representative, candidate);
                        }
                groups[i] = representative;
                aliases[representative].push_back(i);
                if (representative == i)
                    cells[cell].push_back(i);
            }
            std::map<std::pair<unsigned, unsigned>, unsigned> capEdges;
            std::size_t capCount = 0;
            for (const auto& t : triangles)
            {
                osg::Vec3d normal = (vertices[t[1]] - vertices[t[0]]) ^ (vertices[t[2]] - vertices[t[0]]);
                if (normal.normalize() <= 1e-12)
                    continue;
                const double top = std::max({vertices[t[0]] * axis, vertices[t[1]] * axis, vertices[t[2]] * axis});
                if (top >= low + .2 * (high - low) || normal * axis >= -.3)
                    continue;
                ++capCount;
                for (unsigned i = 0; i < 3; ++i)
                {
                    unsigned a = groups[t[i]], b = groups[t[(i + 1) % 3]];
                    if (a == b)
                        return {};
                    if (a > b)
                        std::swap(a, b);
                    if (++capEdges[{a, b}] > 2)
                        return {};
                }
            }
            std::map<unsigned, std::vector<unsigned>> boundary;
            for (const auto& [edge, incidence] : capEdges)
                if (incidence == 1)
                {
                    boundary[edge.first].push_back(edge.second);
                    boundary[edge.second].push_back(edge.first);
                }
            if (boundary.size() < 3 || boundary.size() > 4096)
                return {};
            for (const auto& [index, neighbors] : boundary)
                if (neighbors.size() != 2)
                    return {};
            std::vector<unsigned> loop;
            const unsigned start = boundary.begin()->first;
            unsigned current = start, previous = std::numeric_limits<unsigned>::max();
            do
            {
                if (std::find(loop.begin(), loop.end(), current) != loop.end())
                    return {};
                loop.push_back(current);
                const auto& next = boundary.at(current);
                const unsigned chosen = next[0] == previous ? next[1] : next[0];
                previous = current;
                current = chosen;
            } while (current != start);
            if (loop.size() != boundary.size())
                return {}; // Multiple loops/holes are not inferred as one hem.

            auto result = std::unique_ptr<HemCoverage>(new HemCoverage);
            result->mSourceVertexCount = count;
            result->mSourceTriangleCount = triangles.size();
            result->mCapTriangleCount = capCount;
            for (unsigned representative : loop)
                result->mHemVertices.push_back(std::move(aliases[representative]));
            return result;
        }

        const std::vector<std::vector<unsigned>>& getHemVertices() const { return mHemVertices; }
        std::size_t getSourceVertexCount() const { return mSourceVertexCount; }
        std::size_t getSourceTriangleCount() const { return mSourceTriangleCount; }
        std::size_t getCapTriangleCount() const { return mCapTriangleCount; }

        Evaluation evaluate(const std::vector<osg::Vec3d>& hemWorld, const std::vector<osg::Vec3d>& kneeWorld,
            osg::Vec3d distalAxisWorld, double globalGain = 1.) const
        {
            Evaluation result;
            if (hemWorld.size() != mHemVertices.size() || hemWorld.size() < 3 || kneeWorld.size() < 3
                || kneeWorld.size() > 262144 || !finite(distalAxisWorld) || !std::isfinite(globalGain) || globalGain < 0
                || distalAxisWorld.normalize() <= 1e-8)
                return result;
            osg::Vec3d center;
            for (const auto& p : kneeWorld)
            {
                if (!finite(p))
                    return result;
                center += p / static_cast<double>(kneeWorld.size());
            }
            for (const auto& p : hemWorld)
                if (!finite(p))
                    return result;
            const auto& axis = distalAxisWorld;
            unsigned least = 0;
            for (unsigned i = 1; i < 3; ++i)
                if (std::abs(axis[i]) < std::abs(axis[least]))
                    least = i;
            osg::Vec3d arbitrary;
            arbitrary[least] = 1;
            osg::Vec3d u = axis ^ arbitrary;
            u.normalize();
            const osg::Vec3d v = axis ^ u;
            std::vector<std::array<double, 2>> polygon;
            polygon.reserve(hemWorld.size());
            double minHem = std::numeric_limits<double>::infinity();
            for (const auto& p : hemWorld)
            {
                const auto d = p - center;
                polygon.push_back({d * u, d * v});
                minHem = std::min(minHem, d * axis);
            }
            double boundaryDistance2 = std::numeric_limits<double>::infinity();
            bool inside = false;
            for (std::size_t i = 0; i < polygon.size(); ++i)
            {
                const auto& a = polygon[i];
                const auto& b = polygon[(i + 1) % polygon.size()];
                const double dx = b[0] - a[0], dy = b[1] - a[1];
                const double edge2 = dx * dx + dy * dy;
                const double t = edge2 > 1e-24 ? std::clamp(-(a[0] * dx + a[1] * dy) / edge2, 0., 1.) : 0.;
                const double x = a[0] + t * dx, y = a[1] + t * dy;
                boundaryDistance2 = std::min(boundaryDistance2, x * x + y * y);
                if ((a[1] > 0) != (b[1] > 0) && 0 < a[0] - a[1] * dx / dy)
                    inside = !inside;
            }
            double minKnee = std::numeric_limits<double>::infinity(), maxKnee = -minKnee, radius2 = 0;
            for (const auto& p : kneeWorld)
            {
                const auto d = p - center;
                const double axial = d * axis;
                minKnee = std::min(minKnee, axial);
                maxKnee = std::max(maxKnee, axial);
                radius2 = std::max(radius2, (d - axis * axial).length2());
            }
            if (!(maxKnee - minKnee > 1e-8) || !std::isfinite(boundaryDistance2))
                return result;
            result.centerInside = inside;
            result.radius = std::sqrt(radius2);
            result.centerDistance = (inside ? 1. : -1.) * std::sqrt(boundaryDistance2);
            result.hemClearance = result.centerDistance - result.radius;
            const double t = std::clamp(result.hemClearance / FootprintBand, 0., 1.);
            result.footprintGain = t * t * (3 - 2 * t);
            result.appliedGain = std::clamp(globalGain, 0., 1.) * result.footprintGain;
            result.fullCutDistance = minHem - Margin;
            const double noCut = std::min(minKnee - Margin, result.fullCutDistance);
            result.cutDistance = noCut + result.appliedGain * (result.fullCutDistance - noCut);
            result.worldPlane.set(axis.x(), axis.y(), axis.z(), -(center * axis + result.cutDistance));
            std::size_t kept = 0;
            for (const auto& p : kneeWorld)
                kept += (p - center) * axis >= result.cutDistance;
            result.visibleFraction = static_cast<double>(kept) / kneeWorld.size();
            result.visibleAxialFraction = std::clamp((maxKnee - result.cutDistance) / (maxKnee - minKnee), 0., 1.);
            result.valid = std::isfinite(result.cutDistance) && std::isfinite(result.worldPlane.w());
            return result;
        }

    private:
        static bool finite(const osg::Vec3d& p)
        {
            return std::isfinite(p.x()) && std::isfinite(p.y()) && std::isfinite(p.z())
                && std::abs(p.x()) < 1e12 && std::abs(p.y()) < 1e12 && std::abs(p.z()) < 1e12;
        }
        std::vector<std::vector<unsigned>> mHemVertices;
        std::size_t mSourceVertexCount = 0;
        std::size_t mSourceTriangleCount = 0;
        std::size_t mCapTriangleCount = 0;
    };
}

#endif
