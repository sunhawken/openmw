#ifndef OPENMW_MWRENDER_DECAPITATIONCONTOUR_H
#define OPENMW_MWRENDER_DECAPITATIONCONTOUR_H

#include <osg/Vec3f>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

namespace MWRender
{
    using CutSegment = std::pair<osg::Vec3f, osg::Vec3f>;

    // Match the strict edge-crossing rule used when clipping the real triangles.
    inline void appendCutSegment(const std::array<osg::Vec3f, 3>& triangle,
        const osg::Vec3f& center, const osg::Vec3f& normal, std::vector<CutSegment>& segments)
    {
        std::array<osg::Vec3f, 3> crossings;
        std::size_t count = 0;
        for (std::size_t i = 0; i < 3; ++i)
        {
            const auto& a = triangle[i];
            const auto& b = triangle[(i + 1) % 3];
            const float da = (a - center) * normal, db = (b - center) * normal;
            if ((da < 0 && db > 0) || (da > 0 && db < 0))
                crossings[count++] = a + (b - a) * (da / (da - db));
        }
        if (count == 2 && (crossings[0] - crossings[1]).length2() > 1e-10f)
            segments.emplace_back(crossings[0], crossings[1]);
    }

    // Shared by candidate validation and the final geometry build: identical seam
    // welding and manifold/closed-loop requirements, with the same tolerances.
    inline int buildCutContours(const std::vector<CutSegment>& segments,
        std::vector<osg::Vec3f>& points, std::vector<std::vector<int>>& loops)
    {
        points.clear();
        loops.clear();
        std::set<std::pair<int, int>> edges;
        const auto weld = [&](const osg::Vec3f& p) {
            for (std::size_t i = 0; i < points.size(); ++i)
                if ((points[i] - p).length2() < .0001f)
                    return int(i);
            points.push_back(p);
            return int(points.size() - 1);
        };
        for (const auto& [a, b] : segments)
        {
            const int x = weld(a), y = weld(b);
            if (x != y)
                edges.emplace(std::min(x, y), std::max(x, y));
        }
        if (edges.empty() || points.size() > 4096)
            return 4;
        std::vector<std::vector<int>> adjacency(points.size());
        for (const auto& [a, b] : edges)
        {
            adjacency[a].push_back(b);
            adjacency[b].push_back(a);
        }
        for (const auto& a : adjacency)
            if (a.size() != 2)
                return 5;
        std::set<int> visited;
        for (std::size_t start = 0; start < points.size(); ++start)
            if (!visited.count(start))
            {
                std::vector<int> loop;
                int previous = -1, current = int(start);
                do
                {
                    if (visited.count(current))
                        return 6;
                    visited.insert(current);
                    loop.push_back(current);
                    const int next = adjacency[current][0] == previous ? adjacency[current][1] : adjacency[current][0];
                    previous = current;
                    current = next;
                } while (current != int(start));
                if (loop.size() < 3)
                    return 7;
                loops.push_back(std::move(loop));
            }
        return 0;
    }

    inline bool hasClosedCutContour(const std::vector<std::array<osg::Vec3f, 3>>& triangles,
        const osg::Vec3f& center, const osg::Vec3f& normal)
    {
        std::vector<CutSegment> segments;
        for (const auto& triangle : triangles)
            appendCutSegment(triangle, center, normal, segments);
        std::vector<osg::Vec3f> points;
        std::vector<std::vector<int>> loops;
        if (buildCutContours(segments, points, loops) != 0)
            return false;
        for (const auto& loop : loops)
        {
            float area = 0.f;
            for (std::size_t i = 0; i < loop.size(); ++i)
            {
                const auto a = points[loop[i]] - center, b = points[loop[(i + 1) % loop.size()]] - center;
                area += (a ^ b) * normal;
                if (a.length() > 40.f)
                    return false;
            }
            if (std::abs(area) < .01f)
                return false;
        }
        return true;
    }
}

#endif
