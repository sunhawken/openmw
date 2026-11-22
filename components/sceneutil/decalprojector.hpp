#ifndef OPENMW_SCENEUTIL_DECALPROJECTOR_H
#define OPENMW_SCENEUTIL_DECALPROJECTOR_H

#include "surfacedecals.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <osg/Vec3d>
#include <queue>
#include <tuple>

namespace SceneUtil
{
    // Project once in the current pose. UVs remain attached to source triangles,
    // never to an old world-space plane or the model's possibly mirrored UVs.
    struct DecalProjector
    {
        using Triangle = std::array<unsigned int, 3>;
        struct Result
        {
            std::vector<SurfaceDecals::Anchor> vertices;
            double distance = std::numeric_limits<double>::infinity();
            osg::Vec3d point, normal;
        };

        static osg::Vec3d closestWeights(
            const osg::Vec3d& p, const osg::Vec3d& a, const osg::Vec3d& b, const osg::Vec3d& c)
        {
            const auto ab = b - a, ac = c - a, ap = p - a;
            const double d1 = ab * ap, d2 = ac * ap;
            if (d1 <= 0 && d2 <= 0)
                return { 1, 0, 0 };
            const auto bp = p - b;
            const double d3 = ab * bp, d4 = ac * bp;
            if (d3 >= 0 && d4 <= d3)
                return { 0, 1, 0 };
            const double vc = d1 * d4 - d3 * d2;
            if (vc <= 0 && d1 >= 0 && d3 <= 0)
            {
                const double v = d1 / (d1 - d3);
                return { 1 - v, v, 0 };
            }
            const auto cp = p - c;
            const double d5 = ab * cp, d6 = ac * cp;
            if (d6 >= 0 && d5 <= d6)
                return { 0, 0, 1 };
            const double vb = d5 * d2 - d1 * d6;
            if (vb <= 0 && d2 >= 0 && d6 <= 0)
            {
                const double w = d2 / (d2 - d6);
                return { 1 - w, 0, w };
            }
            const double va = d3 * d6 - d5 * d4;
            if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0)
            {
                const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                return { 0, 1 - w, w };
            }
            const double sum = va + vb + vc;
            if (std::abs(sum) < 1e-20)
                return { 1, 0, 0 };
            return { va / sum, vb / sum, vc / sum };
        }

        static Result project(const std::vector<osg::Vec3d>& positions, const std::vector<Triangle>& triangles,
            const osg::Vec3d& contact, osg::Vec3d outward, osg::Vec3d tangent, double width, double height,
            double depth = 4.0, double maxDistance = 16.0)
        {
            Result out;
            const auto finite = [](const osg::Vec3d& v) {
                return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
            };
            if (!finite(contact) || !finite(outward) || !finite(tangent) || !std::isfinite(width)
                || !std::isfinite(height) || !std::isfinite(depth) || !std::isfinite(maxDistance) || width <= 0
                || height <= 0 || depth <= 0 || maxDistance <= 0 || outward.normalize() < 1e-9
                || triangles.size() > 200000)
                return out;
            std::vector<osg::Vec3d> normals(triangles.size());
            std::size_t seed = triangles.size();
            for (std::size_t i = 0; i < triangles.size(); ++i)
            {
                const auto& t = triangles[i];
                if (t[0] >= positions.size() || t[1] >= positions.size() || t[2] >= positions.size())
                    continue;
                const auto& a = positions[t[0]];
                const auto& b = positions[t[1]];
                const auto& c = positions[t[2]];
                if (!finite(a) || !finite(b) || !finite(c))
                    continue;
                auto n = (b - a) ^ (c - a);
                if (n.normalize() < 1e-10)
                    continue;
                normals[i] = n;
                const auto w = closestWeights(contact, a, b, c);
                const auto p = a + (b - a) * w.y() + (c - a) * w.z();
                const double distance = (p - contact).length();
                if (distance < out.distance && std::abs(n * outward) > .15)
                {
                    seed = i;
                    out.distance = distance;
                    out.point = p;
                    out.normal = n;
                }
            }
            if (seed == triangles.size() || out.distance > maxDistance)
                return out;
            if (out.normal * outward < 0)
                out.normal = -out.normal;
            tangent -= out.normal * (tangent * out.normal);
            if (tangent.normalize() < 1e-9)
            {
                tangent = osg::Vec3d(0, 0, 1) ^ out.normal;
                if (tangent.normalize() < 1e-9)
                    tangent = osg::Vec3d(1, 0, 0);
            }
            const osg::Vec3d up = out.normal ^ tangent;
            // Weld coincident seam vertices for topology traversal, not for
            // rendering. Disconnected backfaces never join the hit surface.
            using Key = std::tuple<long long, long long, long long>;
            std::map<Key, unsigned> weld;
            std::vector<unsigned> aliases(positions.size());
            for (std::size_t i = 0; i < positions.size(); ++i)
            {
                if (!finite(positions[i]) || (positions[i] - out.point).length() > 100000)
                    return Result{};
                const auto p = positions[i] - out.point;
                const Key key{ std::llround(p.x() * 1000), std::llround(p.y() * 1000), std::llround(p.z() * 1000) };
                aliases[i] = weld.emplace(key, weld.size()).first->second;
            }
            using Edge = std::pair<unsigned, unsigned>;
            std::map<Edge, std::vector<std::size_t>> edges;
            for (std::size_t i = 0; i < triangles.size(); ++i)
                if (normals[i].length2() > .1)
                    for (unsigned e = 0; e < 3; ++e)
                    {
                        unsigned a = aliases[triangles[i][e]], b = aliases[triangles[i][(e + 1) % 3]];
                        if (a > b)
                            std::swap(a, b);
                        edges[{ a, b }].push_back(i);
                    }
            std::vector<bool> visited(triangles.size());
            std::queue<std::size_t> pending;
            pending.push(seed);
            visited[seed] = true;
            while (!pending.empty())
            {
                const auto i = pending.front();
                pending.pop();
                const auto& t = triangles[i];
                // Keep the same facing hemisphere as the initial winding.
                if (normals[i] * normals[seed] < .15)
                    continue;
                struct Vertex
                {
                    osg::Vec3d p, w;
                };
                std::vector<Vertex> polygon;
                for (unsigned k = 0; k < 3; ++k)
                {
                    osg::Vec3d w;
                    w[k] = 1;
                    polygon.push_back({ positions[t[k]], w });
                }
                const osg::Vec3d axes[]{ tangent, up, out.normal };
                const double half[]{ width * .5, height * .5, depth };
                for (unsigned axis = 0; axis < 3 && !polygon.empty(); ++axis)
                    for (double sign : { -1., 1. })
                    {
                        std::vector<Vertex> clipped;
                        for (std::size_t k = 0; k < polygon.size(); ++k)
                        {
                            const auto& a = polygon[k];
                            const auto& b = polygon[(k + 1) % polygon.size()];
                            const double da = half[axis] - sign * ((a.p - out.point) * axes[axis]);
                            const double db = half[axis] - sign * ((b.p - out.point) * axes[axis]);
                            if (da >= 0)
                                clipped.push_back(a);
                            if ((da >= 0) != (db >= 0))
                            {
                                const double f = da / (da - db);
                                clipped.push_back({ a.p + (b.p - a.p) * f, a.w + (b.w - a.w) * f });
                            }
                        }
                        polygon = std::move(clipped);
                    }
                if (polygon.size() < 3)
                    continue;
                for (std::size_t k = 1; k + 1 < polygon.size(); ++k)
                {
                    if (((polygon[k].p - polygon[0].p) ^ (polygon[k + 1].p - polygon[0].p)).length2() < 1e-16)
                        continue;
                    for (const auto j : { std::size_t(0), k, k + 1 })
                    {
                        const auto& v = polygon[j];
                        const auto d = v.p - out.point;
                        out.vertices.push_back(
                            { t, osg::Vec3f(v.w), osg::Vec2f(.5 + d * tangent / width, .5 + d * up / height) });
                    }
                }
                if (out.vertices.size() > 3072)
                {
                    out.vertices.clear();
                    return out;
                }
                for (unsigned e = 0; e < 3; ++e)
                {
                    unsigned a = aliases[t[e]], b = aliases[t[(e + 1) % 3]];
                    if (a > b)
                        std::swap(a, b);
                    for (auto next : edges[{ a, b }])
                        if (!visited[next])
                        {
                            visited[next] = true;
                            pending.push(next);
                        }
                }
            }
            return out;
        }
    };
}
#endif
