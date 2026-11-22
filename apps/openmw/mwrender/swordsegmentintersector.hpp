#ifndef OPENMW_MWRENDER_SWORDSEGMENTINTERSECTOR_H
#define OPENMW_MWRENDER_SWORDSEGMENTINTERSECTOR_H

// Batched, conservative candidate finder for many world-space line segments in ONE
// IntersectionVisitor traversal. Each drawable's triangles are walked once and tested
// against every segment whose box can touch them.
//
// Contract:
//   ratios()[i] == +infinity : no triangle can be hit by segment i.
//   finite ratios()[i] in [0,1]: a LOWER BOUND (slightly generous) for the ratio of the
//     nearest triangle hit along segment i. The test is a superset of what
//     osgUtil::LineSegmentIntersector reports for triangle primitives, so false positives
//     are possible. The caller confirms with the exact single-ray API.
// This class does no classification (PtrHolder, actors, ...): the configured
// IntersectionVisitor's traversal mask and ignore list handle that, and the exact
// confirmation ray keeps the original nearest-hit semantics.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include <osg/BoundingBox>
#include <osg/BoundingSphere>
#include <osg/Drawable>
#include <osg/Matrixd>
#include <osg/Node>
#include <osg/TriangleFunctor>
#include <osg/Vec3d>
#include <osg/Vec3f>
#include <osgUtil/IntersectionVisitor>

namespace MWRender
{
    class SwordSegmentIntersector : public osgUtil::Intersector
    {
    public:
        struct Stats
        {
            std::size_t drawables = 0; // drawables whose triangles were walked
            std::size_t culledDrawables = 0; // rejected by local bounding box
            std::size_t triangles = 0; // triangles delivered by the functor
            std::size_t worldTriangles = 0; // passed local cull, transformed to world
            std::size_t segmentTests = 0; // exact segment-triangle tests performed
            std::size_t candidateUpdates = 0; // nearest ratio improvements
        };

        SwordSegmentIntersector(
            std::span<const std::pair<osg::Vec3f, osg::Vec3f>> segments, std::span<const unsigned> buckets)
            : osgUtil::Intersector(MODEL, NO_LIMIT)
            , mShared(std::make_shared<Shared>())
        {
            Shared& shared = *mShared;
            const std::size_t count = segments.size();
            shared.segments.resize(count);
            shared.ratios.assign(count, std::numeric_limits<float>::infinity());
            for (std::size_t i = 0; i < count; ++i)
            {
                Segment& segment = shared.segments[i];
                segment.start = osg::Vec3d(segments[i].first);
                const osg::Vec3d end(segments[i].second);
                osg::Vec3d direction = end - segment.start;
                const double length = direction.length();
                if (!std::isfinite(length) || length < 1e-6)
                    continue; // Degenerate: the original ray cannot hit anything either.
                segment.direction = direction / length;
                segment.length = length;
                segment.valid = true;
                segment.box.expandBy(segment.start);
                segment.box.expandBy(end);
                pad(segment.box);
                shared.box.expandBy(segment.box);
            }

            // Group segments by caller bucket id (e.g. interpolation step). A size
            // mismatch puts everything into one bucket; results are still exact.
            std::vector<std::size_t> order(count);
            std::iota(order.begin(), order.end(), std::size_t(0));
            const bool useBuckets = buckets.size() == count;
            if (useBuckets)
                std::stable_sort(
                    order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return buckets[a] < buckets[b]; });
            for (std::size_t k = 0; k < count;)
            {
                Bucket bucket;
                bucket.begin = shared.order.size();
                std::size_t m = k;
                for (; m < count && (!useBuckets || buckets[order[m]] == buckets[order[k]]); ++m)
                {
                    const Segment& segment = shared.segments[order[m]];
                    if (!segment.valid)
                        continue;
                    shared.order.push_back(order[m]);
                    bucket.box.expandBy(segment.box);
                }
                bucket.end = shared.order.size();
                if (bucket.end > bucket.begin)
                    shared.buckets.push_back(bucket);
                k = m;
            }
            mLocalBox = shared.box; // root frame is world (no model matrix)
            mLocalValid = true;
        }

        ~SwordSegmentIntersector() override = default;

        const std::vector<float>& ratios() const { return mShared->ratios; }
        const Stats& stats() const { return mShared->stats; }

        Intersector* clone(osgUtil::IntersectionVisitor& iv) override
        {
            osg::Matrixd localToWorld;
            if (const osg::RefMatrix* model = iv.getModelMatrix())
                localToWorld = *model;
            return new SwordSegmentIntersector(mShared, localToWorld);
        }

        bool enter(const osg::Node& node) override
        {
            if (!mShared->box.valid())
                return false;
            if (!mLocalValid || !node.isCullingActive())
                return true;
            const osg::BoundingSphere& sphere = node.getBound();
            if (!sphere.valid())
                return true;
            const osg::Vec3d center(sphere.center());
            double distance2 = 0.0;
            for (int axis = 0; axis < 3; ++axis)
            {
                const double c = center[axis];
                if (c < mLocalBox._min[axis])
                    distance2 += (mLocalBox._min[axis] - c) * (mLocalBox._min[axis] - c);
                else if (c > mLocalBox._max[axis])
                    distance2 += (c - mLocalBox._max[axis]) * (c - mLocalBox._max[axis]);
            }
            const double radius = double(sphere.radius()) * (1.0 + 1e-5) + 1e-3;
            return distance2 <= radius * radius;
        }

        void leave() override {}

        void intersect(osgUtil::IntersectionVisitor& iv, osg::Drawable* drawable) override
        {
            if (!drawable || mShared->buckets.empty())
                return;
            Stats& stats = mShared->stats;
            if (mLocalValid)
            {
                const osg::BoundingBox& bounds = drawable->getBoundingBox();
                if (bounds.valid())
                {
                    osg::BoundingBoxd local(osg::Vec3d(bounds._min), osg::Vec3d(bounds._max));
                    pad(local);
                    if (!local.intersects(mLocalBox))
                    {
                        ++stats.culledDrawables;
                        return;
                    }
                }
            }
            if (iv.getDoDummyTraversal())
                return;
            ++stats.drawables;
            osg::TriangleFunctor<TriangleSink> functor;
            functor.self = this;
            drawable->accept(functor);
        }

        void reset() override
        {
            osgUtil::Intersector::reset();
            std::fill(mShared->ratios.begin(), mShared->ratios.end(), std::numeric_limits<float>::infinity());
            mShared->stats = Stats{};
        }

        bool containsIntersections() override { return mShared->stats.candidateUpdates != 0; }

    private:
        struct Segment
        {
            osg::Vec3d start;
            osg::Vec3d direction; // unit
            double length = 0.0;
            osg::BoundingBoxd box;
            bool valid = false;
        };

        struct Bucket
        {
            osg::BoundingBoxd box;
            std::size_t begin = 0, end = 0; // range into Shared::order
        };

        struct Shared
        {
            std::vector<Segment> segments;
            std::vector<std::size_t> order; // valid segment indices grouped by bucket
            std::vector<Bucket> buckets;
            osg::BoundingBoxd box; // overall world box
            std::vector<float> ratios;
            Stats stats;
        };

        struct TriangleSink
        {
            SwordSegmentIntersector* self = nullptr;
            void operator()(const osg::Vec3& a, const osg::Vec3& b, const osg::Vec3& c, bool /*temp*/ = false)
            {
                self->triangle(a, b, c);
            }
        };

        SwordSegmentIntersector(const std::shared_ptr<Shared>& shared, const osg::Matrixd& localToWorld)
            : osgUtil::Intersector(MODEL, NO_LIMIT)
            , mShared(shared)
            , mLocalToWorld(localToWorld)
            , mIdentity(localToWorld.isIdentity())
        {
            osg::Matrixd worldToLocal;
            mLocalValid = shared->box.valid() && worldToLocal.invert(localToWorld);
            if (mLocalValid)
            {
                // Conservative local box: 8 transformed corners of the padded world box.
                for (unsigned int i = 0; i < 8; ++i)
                    mLocalBox.expandBy(shared->box.corner(i) * worldToLocal);
                pad(mLocalBox);
                mLocalValid = mLocalBox.valid() && std::isfinite(mLocalBox.radius());
            }
        }

        static void pad(osg::BoundingBoxd& box)
        {
            if (!box.valid())
                return;
            const double extent = (box._max - box._min).length();
            const double margin = 1e-3 + 1e-5 * extent;
            box._min -= osg::Vec3d(margin, margin, margin);
            box._max += osg::Vec3d(margin, margin, margin);
        }

        void triangle(const osg::Vec3& a, const osg::Vec3& b, const osg::Vec3& c)
        {
            Shared& shared = *mShared;
            ++shared.stats.triangles;
            if (mLocalValid)
            {
                osg::BoundingBoxd local;
                local.expandBy(osg::Vec3d(a));
                local.expandBy(osg::Vec3d(b));
                local.expandBy(osg::Vec3d(c));
                if (!local.intersects(mLocalBox))
                    return;
            }
            const osg::Vec3d v0 = mIdentity ? osg::Vec3d(a) : osg::Vec3d(a) * mLocalToWorld;
            const osg::Vec3d v1 = mIdentity ? osg::Vec3d(b) : osg::Vec3d(b) * mLocalToWorld;
            const osg::Vec3d v2 = mIdentity ? osg::Vec3d(c) : osg::Vec3d(c) * mLocalToWorld;
            osg::BoundingBoxd world;
            world.expandBy(v0);
            world.expandBy(v1);
            world.expandBy(v2);
            if (!world.valid() || !std::isfinite(world.radius()))
                return;
            pad(world);
            if (!world.intersects(shared.box))
                return;
            const osg::Vec3d e1 = v1 - v0, e2 = v2 - v0;
            if ((e1 ^ e2).length2() <= 0.0)
                return; // Degenerate triangle: no face for the original ray to hit.
            ++shared.stats.worldTriangles;
            for (const Bucket& bucket : shared.buckets)
            {
                if (!world.intersects(bucket.box))
                    continue;
                for (std::size_t k = bucket.begin; k < bucket.end; ++k)
                {
                    const std::size_t index = shared.order[k];
                    const Segment& segment = shared.segments[index];
                    if (!world.intersects(segment.box))
                        continue;
                    ++shared.stats.segmentTests;
                    double ratio = 0.0;
                    if (!candidate(segment, v0, e1, e2, ratio))
                        continue;
                    float& best = shared.ratios[index];
                    if (float(ratio) < best)
                    {
                        best = float(ratio);
                        ++shared.stats.candidateUpdates;
                    }
                }
            }
        }

        // Generous Moller-Trumbore, both windings. Returns a lower-bound ratio.
        static bool candidate(
            const Segment& segment, const osg::Vec3d& v0, const osg::Vec3d& e1, const osg::Vec3d& e2, double& ratio)
        {
            constexpr double barycentricEps = 1e-4;
            const osg::Vec3d p = segment.direction ^ e2;
            const double det = e1 * p;
            if (!(std::abs(det) > 0.0) || !std::isfinite(det))
                return false; // Exactly parallel: LineSegmentIntersector rejects too.
            const double inverse = 1.0 / det;
            const osg::Vec3d t = segment.start - v0;
            const double u = (t * p) * inverse;
            if (!(u >= -barycentricEps && u <= 1.0 + barycentricEps))
                return false;
            const osg::Vec3d q = t ^ e1;
            const double v = (segment.direction * q) * inverse;
            if (!(v >= -barycentricEps && u + v <= 1.0 + barycentricEps))
                return false;
            const double distance = (e2 * q) * inverse;
            const double distanceEps = 1e-4 * segment.length + 1e-3;
            if (!(distance >= -distanceEps && distance <= segment.length + distanceEps))
                return false;
            ratio = std::clamp((distance - distanceEps) / segment.length, 0.0, 1.0);
            return true;
        }

        std::shared_ptr<Shared> mShared;
        osg::Matrixd mLocalToWorld;
        bool mIdentity = true;
        osg::BoundingBoxd mLocalBox;
        bool mLocalValid = false;
    };
}

#endif
