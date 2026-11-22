#ifndef OPENMW_MWRENDER_DECAPITATIONPLANE_H
#define OPENMW_MWRENDER_DECAPITATIONPLANE_H

#include <osg/Vec3f>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace MWRender
{
    // A fallback for a posed neck whose origin-to-origin axis no longer separates
    // the retained collar from the chin. Never relax the actual surface guards.
    struct AnatomicalCutPlane
    {
        osg::Vec3f normal;
        float distance = 0.f;
        float retainedMaximum = 0.f;
        float neckMinimum = 0.f, neckMaximum = 0.f, headMinimum = 0.f;
        float clearance = 0.f;
    };

    struct AnatomicalCutSearchDiagnostics
    {
        bool invalidInput = false;
        unsigned normalCandidates = 0, axisRejected = 0, intervalRejected = 0;
        unsigned scoreSkipped = 0, contourTests = 0, contourRejected = 0, acceptedCandidates = 0;
        unsigned fallbackNormalCandidates = 0, fallbackContourTests = 0, fallbackSlabTests = 0;
        bool fallbackBudgetExhausted = false;
        float bestClearance = -std::numeric_limits<float>::max();
        float bestLower = 0.f, bestUpper = 0.f;
        osg::Vec3f bestNormal;
    };

    inline bool selectAnatomicalCutPlane(const osg::Vec3f& neckOrigin, const osg::Vec3f& headOrigin,
        osg::Vec3f headUp, osg::Vec3f neckUp, osg::Vec3f headSide,
        const std::vector<osg::Vec3f>& retainedPoints, const std::vector<osg::Vec3f>& neckPoints,
        const std::vector<osg::Vec3f>& headPoints, AnatomicalCutPlane& result,
        const std::function<bool(const osg::Vec3f&, float)>& acceptContour = {},
        AnatomicalCutSearchDiagnostics* diagnostics = nullptr)
    {
        if (diagnostics)
            *diagnostics = {};
        const auto finite = [](const osg::Vec3f& p) {
            return std::isfinite(p.x()) && std::isfinite(p.y()) && std::isfinite(p.z());
        };
        if (neckPoints.empty() || !finite(neckOrigin) || !finite(headOrigin)
            || !finite(headUp) || !finite(neckUp) || !finite(headSide))
        {
            if (diagnostics) diagnostics->invalidInput = true;
            return false;
        }
        for (const auto* points : { &retainedPoints, &neckPoints, &headPoints })
            for (const auto& point : *points)
                if (!finite(point))
                {
                    if (diagnostics) diagnostics->invalidInput = true;
                    return false;
                }
        osg::Vec3f axis = headOrigin - neckOrigin;
        if (axis.normalize() < .1f || headUp.normalize() < 1e-5f || neckUp.normalize() < 1e-5f)
        {
            if (diagnostics) diagnostics->invalidInput = true;
            return false;
        }
        const osg::Vec3f boneDelta = headOrigin - neckOrigin;
        const std::array<osg::Vec3f, 4> bases{ axis, headUp, neckUp, headUp + neckUp };
        constexpr std::array<float, 5> offsets{ 0.f, -.5f, .5f, -1.f, 1.f };
        float bestScore = -std::numeric_limits<float>::max();
        bool found = false;
        struct Candidate
        {
            AnatomicalCutPlane plane;
            float lower = 0.f, upper = 0.f, score = 0.f;
            bool original = false;
            std::vector<float> slabMidpoints;
        };
        std::vector<Candidate> seeds;
        const auto sameNormal = [](const osg::Vec3f& a, const osg::Vec3f& b) {
            return a * b > 1.f - 1e-6f;
        };
        for (auto base : bases)
        {
            if (base.normalize() < 1e-5f)
                continue;
            auto tangent = headSide - base * (headSide * base);
            // Both alternatives are anatomical axes, preserving covariance when
            // the entire actor is rotated in the world.
            if (tangent.normalize() < 1e-5f)
            {
                tangent = headUp - base * (headUp * base);
                if (tangent.normalize() < 1e-5f)
                    continue;
            }
            auto bitangent = base ^ tangent;
            bitangent.normalize();
            for (float u : offsets)
                for (float v : offsets)
                {
                    auto normal = base + tangent * u + bitangent * v;
                    if (diagnostics) ++diagnostics->normalCandidates;
                    if (normal.normalize() < 1e-5f || normal * axis < .25f)
                    {
                        if (diagnostics) ++diagnostics->axisRejected;
                        continue;
                    }
                    const float boneProjection = boneDelta * normal;
                    float retainedMaximum = -std::numeric_limits<float>::max();
                    float neckMinimum = std::numeric_limits<float>::max();
                    float neckMaximum = -std::numeric_limits<float>::max();
                    float headMinimum = std::numeric_limits<float>::max();
                    for (const auto& point : retainedPoints)
                        retainedMaximum = std::max(retainedMaximum, (point - neckOrigin) * normal);
                    for (const auto& point : neckPoints)
                    {
                        const float d = (point - neckOrigin) * normal;
                        neckMinimum = std::min(neckMinimum, d);
                        neckMaximum = std::max(neckMaximum, d);
                    }
                    for (const auto& point : headPoints)
                        headMinimum = std::min(headMinimum, (point - neckOrigin) * normal);
                    const float lower = std::max(retainedMaximum + .15f, neckMinimum + .05f);
                    const float upper = std::min({ headMinimum - .05f, neckMaximum - .05f, boneProjection - .05f });
                    const float clearance = upper - lower;
                    if (diagnostics && std::isfinite(clearance) && clearance > diagnostics->bestClearance)
                    {
                        diagnostics->bestClearance = clearance;
                        diagnostics->bestLower = lower;
                        diagnostics->bestUpper = upper;
                        diagnostics->bestNormal = normal;
                    }
                    if (!std::isfinite(clearance) || clearance < .1f)
                    {
                        if (diagnostics) ++diagnostics->intervalRejected;
                        continue;
                    }
                    // Prefer a generous surface gap, then the original axis when
                    // otherwise equal. Do not use a world-axis tie breaker.
                    const float score = clearance + .01f * (normal * axis);
                    if (score <= bestScore)
                    {
                        if (diagnostics) ++diagnostics->scoreSkipped;
                        continue;
                    }
                    // A projected vertex gap alone does not imply that a tilted
                    // plane cuts a closed ring through the open-ended neck mesh.
                    // At most 100 normals x 3 distances; no full mesh builds here.
                    bool accepted = false;
                    for (float fraction : { .5f, .25f, .75f })
                    {
                        const float distance = lower + clearance * fraction;
                        if (acceptContour)
                        {
                            if (diagnostics) ++diagnostics->contourTests;
                            if (!acceptContour(normal, distance))
                            {
                                if (diagnostics) ++diagnostics->contourRejected;
                                continue;
                            }
                        }
                        bestScore = score;
                        result = { normal, distance, retainedMaximum,
                            neckMinimum, neckMaximum, headMinimum, clearance };
                        found = true;
                        accepted = true;
                        if (diagnostics) ++diagnostics->acceptedCandidates;
                        break;
                    }
                    if (!accepted && acceptContour)
                    {
                        Candidate seed{ { normal, 0.f, retainedMaximum,
                            neckMinimum, neckMaximum, headMinimum, clearance }, lower, upper, score, true, {} };
                        auto duplicate = std::find_if(seeds.begin(), seeds.end(), [&](const Candidate& other) {
                            return sameNormal(other.plane.normal, normal);
                        });
                        if (duplicate == seeds.end()) seeds.push_back(std::move(seed));
                        else if (score > duplicate->score) *duplicate = std::move(seed);
                        std::stable_sort(seeds.begin(), seeds.end(), [](const Candidate& a, const Candidate& b) {
                            return a.score > b.score;
                        });
                        if (seeds.size() > 4) seeds.resize(4);
                    }
                }
        }
        // Without contour validation the seed selection above is final.
        if (found || !acceptContour || seeds.empty())
            return found;

        std::vector<Candidate> candidates = seeds;
        constexpr std::array<float, 5> refinement{ 0.f, -.0625f, .0625f, -.125f, .125f };
        for (const auto& seed : seeds)
        {
            const auto base = seed.plane.normal;
            auto tangent = headSide - base * (headSide * base);
            if (tangent.normalize() < 1e-5f)
            {
                tangent = headUp - base * (headUp * base);
                if (tangent.normalize() < 1e-5f) continue;
            }
            auto bitangent = base ^ tangent;
            bitangent.normalize();
            for (float u : refinement)
                for (float v : refinement)
                {
                    if (u == 0.f && v == 0.f) continue;
                    auto normal = base + tangent * u + bitangent * v;
                    if (diagnostics)
                    {
                        ++diagnostics->normalCandidates;
                        ++diagnostics->fallbackNormalCandidates;
                    }
                    if (normal.normalize() < 1e-5f || normal * axis < .25f)
                    {
                        if (diagnostics) ++diagnostics->axisRejected;
                        continue;
                    }
                    if (std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& other) {
                        return sameNormal(other.plane.normal, normal);
                    })) continue;
                    float retainedMaximum = -std::numeric_limits<float>::max();
                    float neckMinimum = std::numeric_limits<float>::max();
                    float neckMaximum = -std::numeric_limits<float>::max();
                    float headMinimum = std::numeric_limits<float>::max();
                    for (const auto& point : retainedPoints)
                        retainedMaximum = std::max(retainedMaximum, (point - neckOrigin) * normal);
                    for (const auto& point : neckPoints)
                    {
                        const float d = (point - neckOrigin) * normal;
                        neckMinimum = std::min(neckMinimum, d);
                        neckMaximum = std::max(neckMaximum, d);
                    }
                    for (const auto& point : headPoints)
                        headMinimum = std::min(headMinimum, (point - neckOrigin) * normal);
                    const float lower = std::max(retainedMaximum + .15f, neckMinimum + .05f);
                    const float upper = std::min({ headMinimum - .05f, neckMaximum - .05f,
                        boneDelta * normal - .05f });
                    const float clearance = upper - lower;
                    if (diagnostics && std::isfinite(clearance) && clearance > diagnostics->bestClearance)
                    {
                        diagnostics->bestClearance = clearance;
                        diagnostics->bestLower = lower;
                        diagnostics->bestUpper = upper;
                        diagnostics->bestNormal = normal;
                    }
                    if (!std::isfinite(clearance) || clearance < .1f)
                    {
                        if (diagnostics) ++diagnostics->intervalRejected;
                        continue;
                    }
                    candidates.push_back({ { normal, 0.f, retainedMaximum,
                        neckMinimum, neckMaximum, headMinimum, clearance }, lower, upper,
                        clearance + .01f * (normal * axis), false, {} });
                }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.score > b.score;
        });
        constexpr unsigned maxContourTests = 1024;
        unsigned contourTests = 0;
        const auto tryDistance = [&](const Candidate& candidate, float distance, bool slab) {
            if (contourTests >= maxContourTests)
            {
                if (diagnostics) diagnostics->fallbackBudgetExhausted = true;
                return false;
            }
            ++contourTests;
            if (diagnostics)
            {
                ++diagnostics->contourTests;
                ++diagnostics->fallbackContourTests;
                if (slab) ++diagnostics->fallbackSlabTests;
            }
            if (!acceptContour(candidate.plane.normal, distance))
            {
                if (diagnostics) ++diagnostics->contourRejected;
                return false;
            }
            result = candidate.plane;
            result.distance = distance;
            if (diagnostics) ++diagnostics->acceptedCandidates;
            return true;
        };
        // Cover the whole small normal stencil with the cheap seed offsets
        // before spending any budget on critical-height interval refinement.
        for (const auto& candidate : candidates)
            if (!candidate.original)
                for (float fraction : { .5f, .25f, .75f })
                    if (tryDistance(candidate, candidate.lower + candidate.plane.clearance * fraction, false))
                        return true;

        constexpr std::size_t maxSlabsPerNormal = 64;
        for (auto& candidate : candidates)
        {
            std::vector<float> critical{ candidate.lower, candidate.upper };
            // The caller's Neck points include every exact vertex used by its
            // contour triangles. Unreferenced points merely subdivide cells;
            // every proposed midpoint still goes through the real oracle.
            for (const auto& point : neckPoints)
            {
                const float projection = (point - neckOrigin) * candidate.plane.normal;
                if (projection > candidate.lower && projection < candidate.upper)
                    critical.push_back(projection);
            }
            std::sort(critical.begin(), critical.end());
            critical.erase(std::unique(critical.begin(), critical.end(), [](float a, float b) {
                return std::abs(a - b) < 1e-5f;
            }), critical.end());
            // Keep both guard endpoints even if a near-identical projected
            // vertex was merged at an end of the sorted list.
            critical.front() = candidate.lower;
            critical.back() = candidate.upper;
            std::vector<std::pair<float, float>> slabs;
            for (std::size_t i = 1; i < critical.size(); ++i)
            {
                const float width = critical[i] - critical[i - 1];
                if (width <= 1e-5f) continue;
                const float midpoint = critical[i - 1] + width * .5f;
                bool alreadyTested = false;
                for (float fraction : { .5f, .25f, .75f })
                    alreadyTested |= std::abs(midpoint - candidate.lower - candidate.plane.clearance * fraction) < 1e-5f;
                if (!alreadyTested) slabs.emplace_back(width, midpoint);
            }
            std::stable_sort(slabs.begin(), slabs.end(), [](const auto& a, const auto& b) {
                return a.first > b.first;
            });
            if (slabs.size() > maxSlabsPerNormal) slabs.resize(maxSlabsPerNormal);
            for (const auto& [width, midpoint] : slabs)
                candidate.slabMidpoints.push_back(midpoint);
        }
        // Round-robin widest cells so one difficult normal cannot consume the
        // entire budget. This is a bounded fallback, not a complete plane solver.
        for (std::size_t slab = 0; slab < maxSlabsPerNormal; ++slab)
            for (const auto& candidate : candidates)
                if (slab < candidate.slabMidpoints.size())
                {
                    if (tryDistance(candidate, candidate.slabMidpoints[slab], true)) return true;
                    if (contourTests >= maxContourTests)
                    {
                        if (diagnostics) diagnostics->fallbackBudgetExhausted = true;
                        return false;
                    }
                }
        return false;
    }
}

#endif
