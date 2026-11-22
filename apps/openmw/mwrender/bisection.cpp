#include "bisection.hpp"

#include <osg/BufferObject>
#include <osgUtil/CullVisitor>

namespace MWRender
{
    namespace
    {
        using Vertex = DecapitationGeometry::Vertex;
        struct BoundVertex
        {
            Vertex vertex;
            std::array<unsigned, 3> indices{};
            osg::Vec3f weights;
            bool fixed = false;
            BoundVertex mix(const BoundVertex& other, float t) const
            {
                return { vertex.mix(other.vertex, t), indices, weights + (other.weights - weights) * t, true };
            }
        };

        bool finite(const osg::Vec3f& point)
        {
            return std::isfinite(point.x()) && std::isfinite(point.y()) && std::isfinite(point.z());
        }

        std::vector<BoundVertex> clip(const std::vector<BoundVertex>& polygon,
            const osg::Vec3f& center, const osg::Vec3f& normal, unsigned side)
        {
            std::vector<BoundVertex> result;
            for (std::size_t i = 0; i < polygon.size(); ++i)
            {
                const auto& a = polygon[i];
                const auto& b = polygon[(i + 1) % polygon.size()];
                const float da = (a.vertex.p - center) * normal, db = (b.vertex.p - center) * normal;
                if (side ? da >= 0 : da <= 0)
                    result.push_back(a);
                if ((da < 0 && db > 0) || (da > 0 && db < 0))
                    result.push_back(a.mix(b, da / (da - db)));
            }
            return result;
        }

        void append(std::vector<BoundVertex>& out, const std::vector<BoundVertex>& polygon)
        {
            for (std::size_t i = 1; i + 1 < polygon.size(); ++i)
                if (((polygon[i].vertex.p - polygon[0].vertex.p)
                        ^ (polygon[i + 1].vertex.p - polygon[0].vertex.p)).length2() > 1e-12f)
                {
                    out.push_back(polygon[0]);
                    out.push_back(polygon[i]);
                    out.push_back(polygon[i + 1]);
                }
        }

        osg::Vec3f interpolate(const osg::Vec3Array& points, const BoundVertex& vertex)
        {
            const auto& a = points[vertex.indices[0]];
            return a + (points[vertex.indices[1]] - a) * vertex.weights.y()
                + (points[vertex.indices[2]] - a) * vertex.weights.z();
        }
    }

    struct BisectionGeometry::Source
    {
        DecapitationGeometry::Snapshot snapshot;
        osg::ref_ptr<SceneUtil::RigGeometry> rig;
        std::vector<osg::observer_ptr<osg::Node>> sourcePath;
        BoneWorld bones;
        std::array<std::map<std::string, osg::Matrixf>, 2> overrides;
        std::array<osg::ref_ptr<osg::Geometry>, 2> sampled;
        std::array<osg::Matrixf, 2> toLocal;
        std::array<unsigned, 2> traversal{ ~0u, ~0u };
        std::vector<int> protectedSide;
        std::array<osg::Matrixf, 2> initialInverse;

        int triangleSide(const std::array<unsigned, 3>& tri) const
        {
            int side = -1;
            for (unsigned index : tri)
            {
                if (index >= protectedSide.size())
                    return -2;
                if (protectedSide[index] < 0)
                    continue;
                if (side >= 0 && side != protectedSide[index])
                    return -2;
                side = protectedSide[index];
            }
            return side;
        }

        osg::Geometry* sample(unsigned side, unsigned frame)
        {
            if (traversal[side] == frame)
                return sampled[side];
            traversal[side] = frame;
            sampled[side] = nullptr;
            const auto anchor = bones(side ? "Bip01 Spine1" : "Bip01 Pelvis");
            if (!anchor)
                return nullptr;
            toLocal[side] = osg::Matrixf::inverse(*anchor);
            if (rig)
            {
                osg::NodePath path;
                std::vector<osg::ref_ptr<osg::Node>> keepAlive;
                path.reserve(sourcePath.size());
                keepAlive.reserve(sourcePath.size());
                for (const auto& weak : sourcePath)
                {
                    osg::ref_ptr<osg::Node> node;
                    if (!weak.lock(node)) return nullptr;
                    path.push_back(node.get());
                    keepAlive.push_back(std::move(node));
                }
                const BoneWorld posed = [&](std::string_view name) -> std::optional<osg::Matrixf> {
                    const auto found = overrides[side].find(std::string(name));
                    return found == overrides[side].end() ? bones(name)
                                                         : std::optional<osg::Matrixf>(found->second * *anchor);
                };
                sampled[side] = rig->snapshotWorldGeometry(path, posed, false, true);
            }
            return sampled[side];
        }
    };

    class BisectionGeometry::Surface final : public osg::Geometry
    {
    public:
        Surface() = default;
        Surface(const Surface& copy, const osg::CopyOp& op)
            : osg::Geometry(copy, op), source(copy.source), vertices(copy.vertices), side(copy.side), decal(copy.decal)
        {
        }
        META_Object(MWRender, Surface)

        std::shared_ptr<Source> source;
        std::vector<BoundVertex> vertices;
        unsigned side = 0;
        bool decal = false;
        std::array<osg::ref_ptr<osg::Geometry>, 2> buffers;
        unsigned lastTraversal = ~0u;

        void prepare()
        {
            for (auto& buffer : buffers)
            {
                // Arrays are private, including their buffer objects. Materials
                // remain immutable and shared with this actor's prepared surface.
                buffer = new osg::Geometry(*this, osg::CopyOp::DEEP_COPY_ARRAYS | osg::CopyOp::DEEP_COPY_PRIMITIVES);
                buffer->setCullingActive(false);
                osg::ref_ptr<osg::VertexBufferObject> vbo = new osg::VertexBufferObject;
                vbo->setUsage(GL_DYNAMIC_DRAW_ARB);
                buffer->getVertexArray()->setVertexBufferObject(vbo);
                if (buffer->getNormalArray())
                    buffer->getNormalArray()->setVertexBufferObject(vbo);
                if (buffer->getTexCoordArray(7))
                    buffer->getTexCoordArray(7)->setVertexBufferObject(vbo);
            }
            lastTraversal = ~0u;
        }

        void accept(osg::NodeVisitor& visitor) override
        {
            if (!visitor.validNodeMask(*this))
                return;
            if (!dynamic_cast<osgUtil::CullVisitor*>(&visitor))
            {
                osg::Geometry::accept(visitor);
                return;
            }
            const unsigned frame = visitor.getTraversalNumber();
            auto& buffer = buffers[frame % 2];
            if (!buffer)
                return;
            if (lastTraversal != frame && source->rig)
            {
                const auto* sampled = source->sample(side, frame);
                if (sampled)
                {
                    const auto* p = dynamic_cast<const osg::Vec3Array*>(sampled->getVertexArray());
                    const auto* n = dynamic_cast<const osg::Vec3Array*>(sampled->getNormalArray());
                    const auto* t = dynamic_cast<const osg::Vec4Array*>(sampled->getTexCoordArray(7));
                    auto* outP = static_cast<osg::Vec3Array*>(buffer->getVertexArray());
                    auto* outN = static_cast<osg::Vec3Array*>(buffer->getNormalArray());
                    auto* outT = dynamic_cast<osg::Vec4Array*>(buffer->getTexCoordArray(7));
                    if (p)
                        for (std::size_t i = 0; i < vertices.size(); ++i)
                        {
                            const auto& vertex = vertices[i];
                            // The intersection ring is rigid in the cap's frame.
                            // Interior sleeve/leg vertices retain full rig motion.
                            if (vertex.fixed)
                                continue;
                            (*outP)[i] = interpolate(*p, vertex) * source->toLocal[side];
                            if (n && n->size() == p->size())
                            {
                                (*outN)[i] = osg::Matrixf::transform3x3(interpolate(*n, vertex), source->toLocal[side]);
                                (*outN)[i].normalize();
                            }
                            if (outT && t && t->size() == p->size())
                            {
                                osg::Vec4f tangent;
                                for (unsigned j = 0; j < 3; ++j)
                                    tangent += (*t)[vertex.indices[j]] * vertex.weights[j];
                                auto xyz = osg::Matrixf::transform3x3(
                                    osg::Vec3f(tangent.x(), tangent.y(), tangent.z()), source->toLocal[side]);
                                xyz.normalize();
                                (*outT)[i] = osg::Vec4f(xyz, tangent.w());
                            }
                        }
                    outP->dirty();
                    outN->dirty();
                    if (outT)
                        outT->dirty();
                    buffer->dirtyBound();
                }
                lastTraversal = frame;
            }
            buffer->accept(visitor);
        }
    };

    namespace
    {
        osg::ref_ptr<BisectionGeometry::Surface> makeSurface(const std::shared_ptr<BisectionGeometry::Source>& source,
            std::vector<BoundVertex> bound, unsigned side, osg::StateSet* state,
            const std::array<int, 8>& texDimensions = {}, bool decal = false)
        {
            std::vector<Vertex> vertices;
            vertices.reserve(bound.size());
            for (const auto& vertex : bound)
                vertices.push_back(vertex.vertex);
            const auto geometry = DecapitationGeometry::makeGeometry(vertices, state, source->initialInverse[side], texDimensions);
            osg::ref_ptr<BisectionGeometry::Surface> result = new BisectionGeometry::Surface;
            result->setVertexArray(geometry->getVertexArray());
            result->setNormalArray(geometry->getNormalArray(), osg::Array::BIND_PER_VERTEX);
            result->setColorArray(geometry->getColorArray(), osg::Array::BIND_PER_VERTEX);
            for (unsigned unit = 0; unit < 8; ++unit)
                if (geometry->getTexCoordArray(unit))
                    result->setTexCoordArray(unit, geometry->getTexCoordArray(unit));
            result->setPrimitiveSetList(geometry->getPrimitiveSetList());
            result->setStateSet(state);
            result->setUseDisplayList(false);
            result->setUseVertexBufferObjects(true);
            result->setCullingActive(false);
            result->source = source;
            result->vertices = std::move(bound);
            result->side = side;
            result->decal = decal;
            return result;
        }
    }

    BisectionGeometry::BisectionGeometry()
    {
        lower->setName("Brutal Bisection Lower");
        upper->setName("Brutal Bisection Upper");
        lower->setCullingActive(false);
        upper->setCullingActive(false);
    }

    BisectionGeometry::~BisectionGeometry() { restore(); }

    void BisectionGeometry::restore()
    {
        if (!committed)
            return;
        for (const auto& [node, mask] : originals)
            node->setNodeMask(mask);
        for (const auto& group : { lower, upper })
            while (group->getNumParents())
                group->getParent(0)->removeChild(group);
        committed = false;
    }

    bool BisectionGeometry::build(const std::vector<DecapitationGeometry::Snapshot>& snapshots,
        const BoneWorld& bones, const BoneWorld& liveBones, const BoneSide& boneSide, const LimbSide& limbSide,
        const osg::Matrixf& pelvis, const osg::Matrixf& spine, const osg::Vec3f& axis,
        float offset, osg::StateSet* lowerCap, osg::StateSet* upperCap)
    {
        failure = 2;
        if (snapshots.empty() || snapshots.size() > 64 || !finite(axis) || std::abs(axis.length2() - 1) > 1e-4f)
            return false;
        inverse = { osg::Matrixf::inverse(pelvis), osg::Matrixf::inverse(spine) };
        spineInPelvis = spine * inverse[0];
        cutDistance = offset;
        center = spine.getTrans() + axis * offset;
        normal = axis;
        std::vector<CutSegment> segments;
        std::size_t totalVertices = 0;
        for (const auto& snapshot : snapshots)
        {
            auto source = std::make_shared<Source>();
            source->snapshot = snapshot;
            for (auto* node : snapshot.sourcePath) source->sourcePath.emplace_back(node);
            source->snapshot.sourcePath.clear();
            source->bones = liveBones;
            source->initialInverse = inverse;
            source->rig = dynamic_cast<SceneUtil::RigGeometry*>(snapshot.sourceDrawable.get());
            auto* geometry = snapshot.geometry.get();
            const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray());
            const auto* normals = dynamic_cast<const osg::Vec3Array*>(geometry->getNormalArray());
            const auto* colors = dynamic_cast<const osg::Vec4Array*>(geometry->getColorArray());
            if (!positions || positions->empty() || positions->size() > 65535
                || (totalVertices += positions->size()) > 131072 || snapshot.sourcePath.empty()
                || dynamic_cast<SceneUtil::MorphGeometry*>(snapshot.sourceDrawable.get()))
                return false;
            source->protectedSide.resize(positions->size(), -1);
            for (const auto& position : *positions)
                if (!finite(position))
                    return false;
            if (source->rig)
                for (const auto& name : source->rig->getInfluencingBoneNames())
                {
                    const auto world = bones(name);
                    const int owner = boneSide(name), limb = limbSide(name);
                    if (!world || owner < 0)
                    { failure = 13; return false; }
                    source->overrides[1 - owner].emplace(name, *world * inverse[1 - owner]);
                    if (limb < 0)
                        continue;
                    for (unsigned index : source->rig->getVerticesInfluencedBy(name))
                    {
                        if (index >= positions->size())
                            return false;
                        auto& value = source->protectedSide[index];
                        if (value >= 0 && value != limb)
                        { failure = 13; return false; }
                        value = limb;
                    }
                }
            std::array<int, 8> dimensions{};
            for (unsigned unit = 0; unit < 8; ++unit)
                if (const auto* array = geometry->getTexCoordArray(unit); array && array->getNumElements() == positions->size())
                {
                    if (dynamic_cast<const osg::Vec2Array*>(array)) dimensions[unit] = 2;
                    else if (dynamic_cast<const osg::Vec3Array*>(array)) dimensions[unit] = 3;
                    else if (dynamic_cast<const osg::Vec4Array*>(array)) dimensions[unit] = 4;
                }
            osg::TriangleIndexFunctor<DecapitationGeometry::TriangleList> triangles;
            geometry->accept(triangles);
            std::array<std::vector<BoundVertex>, 2> halves;
            for (const auto& tri : triangles.triangles)
            {
                if (++sourceTriangles > 131072 || tri[0] >= positions->size()
                    || tri[1] >= positions->size() || tri[2] >= positions->size())
                    return false;
                const int protectedSide = source->triangleSide(tri);
                if (protectedSide == -2)
                { failure = 13; return false; }
                std::vector<BoundVertex> input;
                osg::Vec3f face = ((*positions)[tri[1]] - (*positions)[tri[0]]) ^ ((*positions)[tri[2]] - (*positions)[tri[0]]);
                face.normalize();
                for (unsigned j = 0; j < 3; ++j)
                {
                    const auto index = tri[j];
                    Vertex vertex{ (*positions)[index], normals && normals->size() == positions->size() ? (*normals)[index] : face,
                        {}, colors && !colors->empty() ? (*colors)[colors->size() == positions->size() ? index : 0] : osg::Vec4f(1, 1, 1, 1) };
                    for (unsigned unit = 0; unit < 8; ++unit)
                    {
                        const auto* array = geometry->getTexCoordArray(unit);
                        if (dimensions[unit] == 2)
                        { const auto t = (*static_cast<const osg::Vec2Array*>(array))[index]; vertex.tex[unit].set(t.x(), t.y(), 0, 0); }
                        else if (dimensions[unit] == 3)
                            vertex.tex[unit] = osg::Vec4f((*static_cast<const osg::Vec3Array*>(array))[index], 0);
                        else if (dimensions[unit] == 4)
                            vertex.tex[unit] = (*static_cast<const osg::Vec4Array*>(array))[index];
                    }
                    vertex.uv.set(vertex.tex[0].x(), vertex.tex[0].y());
                    osg::Vec3f weight; weight[j] = 1;
                    input.push_back({ vertex, tri, weight, false });
                }
                if (protectedSide >= 0)
                {
                    ++protectedTriangles;
                    append(halves[protectedSide], input);
                    continue;
                }
                // A vertex exactly on the plane creates an ambiguous contour.
                // The caller searches small anatomical offsets before committing.
                for (const auto& vertex : input)
                    if (std::abs((vertex.vertex.p - center) * normal) < .0001f)
                    { failure = 3; return false; }
                appendCutSegment({ input[0].vertex.p, input[1].vertex.p, input[2].vertex.p }, center, normal, segments);
                for (unsigned side = 0; side < 2; ++side)
                    append(halves[side], clip(input, center, normal, side));
            }
            sources.push_back(source);
            for (unsigned side = 0; side < 2; ++side)
                if (!halves[side].empty())
                {
                    auto surface = makeSurface(source, std::move(halves[side]), side, geometry->getStateSet(), dimensions);
                    resultTriangles += surface->vertices.size() / 3;
                    articulatedSurfaces += bool(source->rig);
                    const auto* local = static_cast<const osg::Vec3Array*>(surface->getVertexArray());
                    for (std::size_t i = 0; i < local->size(); ++i)
                        reconstructionError = std::max(reconstructionError,
                            ((*local)[i] * (side ? spine : pelvis) - surface->vertices[i].vertex.p).length());
                    osg::ref_ptr<osg::Node> material = surface;
                    for (auto state = snapshot.statePath.rbegin(); state != snapshot.statePath.rend(); ++state)
                    {
                        osg::ref_ptr<osg::Group> group = new osg::Group;
                        group->setStateSet(new osg::StateSet(**state, osg::CopyOp::DEEP_COPY_USERDATA));
                        group->addChild(material);
                        material = group;
                    }
                    (side ? upper : lower)->addChild(material);
                    surfaces.push_back(surface);
                }
        }
        std::vector<osg::Vec3f> points;
        std::vector<std::vector<int>> loops;
        if (const int error = buildCutContours(segments, points, loops))
        { failure = error; return false; }
        osg::Vec3f u = normal ^ osg::Vec3f(0, 0, 1);
        if (u.length2() < .01f) u = normal ^ osg::Vec3f(0, 1, 0);
        u.normalize();
        const auto v = normal ^ u;
        const auto xy = [&](int index) { const auto p = points[index] - center; return osg::Vec2f(p * u, p * v); };
        const auto contains = [&](const std::vector<int>& loop, const osg::Vec2f& point) {
            bool inside = false;
            for (std::size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++)
            {
                const auto a = xy(loop[i]), b = xy(loop[j]);
                if ((a.y() > point.y()) != (b.y() > point.y())
                    && point.x() < (b.x() - a.x()) * (point.y() - a.y()) / (b.y() - a.y()) + a.x())
                    inside = !inside;
            }
            return inside;
        };
        std::vector<osg::Vec3f> exteriorPoints;
        for (std::size_t li = 0; li < loops.size(); ++li)
        {
            bool nested = false;
            for (std::size_t j = 0; j < loops.size(); ++j)
                if (j != li && contains(loops[j], xy(loops[li][0]))) nested = true;
            if (nested) continue;
            auto loop = loops[li];
            float area = 0, extent = 0;
            for (std::size_t i = 0; i < loop.size(); ++i)
            {
                const auto a = xy(loop[i]), b = xy(loop[(i + 1) % loop.size()]);
                area += a.x() * b.y() - b.x() * a.y();
                extent = std::max(extent, a.length());
            }
            if (std::abs(area) < .01f || extent > 80.f)
            { failure = 8; return false; }
            if (area < 0) std::reverse(loop.begin(), loop.end());
            osg::ref_ptr<osg::Geometry> polygon = new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> ring = new osg::Vec3Array;
            for (int index : loop) { ring->push_back(points[index]); exteriorPoints.push_back(points[index]); }
            polygon->setVertexArray(ring);
            polygon->addPrimitiveSet(new osg::DrawArrays(GL_POLYGON, 0, ring->size()));
            osgUtil::Tessellator tessellator;
            tessellator.setTessellationType(osgUtil::Tessellator::TESS_TYPE_GEOMETRY);
            tessellator.setTessellationNormal(normal);
            tessellator.retessellatePolygons(*polygon);
            osg::TriangleIndexFunctor<DecapitationGeometry::TriangleList> triangles;
            polygon->accept(triangles);
            if (triangles.triangles.empty()) { failure = 9; return false; }
            const auto* coordinates = static_cast<const osg::Vec3Array*>(polygon->getVertexArray());
            for (unsigned side = 0; side < 2; ++side)
            {
                std::vector<Vertex> cap;
                for (auto tri : triangles.triangles)
                {
                    if ((((*coordinates)[tri[1]] - (*coordinates)[tri[0]])
                            ^ ((*coordinates)[tri[2]] - (*coordinates)[tri[0]])) * normal < 0)
                        std::swap(tri[1], tri[2]);
                    if (side) std::swap(tri[1], tri[2]);
                    for (unsigned index : tri)
                    {
                        const auto p = (*coordinates)[index], delta = p - center;
                        cap.push_back({ p, side ? -normal : normal,
                            osg::Vec2f(.5f + delta * u / (2 * extent), .5f + delta * v / (2 * extent)), osg::Vec4f(1, 1, 1, 1) });
                    }
                }
                (side ? upper : lower)->addChild(DecapitationGeometry::makeGeometry(
                    cap, side ? upperCap : lowerCap, inverse[side]));
                capTriangles += cap.size() / 3;
            }
            ++capLoops;
        }
        if (exteriorPoints.empty() || reconstructionError >= .01f) { failure = 10; return false; }
        osg::Vec3f surfaceCenter;
        for (const auto& point : exteriorPoints) surfaceCenter += point - center;
        surfaceCenter = center + surfaceCenter / float(exteriorPoints.size());
        for (const auto& point : exteriorPoints) surfaceRadius = std::max(surfaceRadius, (point - surfaceCenter).length());
        for (unsigned side = 0; side < 2; ++side)
        {
            surfaceCenters[side] = surfaceCenter * inverse[side];
            surfaceU[side] = osg::Matrixf::transform3x3(u, inverse[side]);
            surfaceV[side] = osg::Matrixf::transform3x3(side ? -v : v, inverse[side]);
        }
        failure = 0;
        return capLoops > 0 && capTriangles > 0;
    }

    void BisectionGeometry::addDecal(osg::Drawable* drawable, const SceneUtil::SurfaceDecals::PatchData& patch)
    {
        const auto found = std::find_if(sources.begin(), sources.end(),
            [&](const auto& source) { return source->snapshot.sourceDrawable.get() == drawable; });
        if (found == sources.end() || patch.vertices.empty() || patch.vertices.size() % 3)
            return;
        const auto& source = *found;
        osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array(patch.vertices.size()),
            normals = new osg::Vec3Array(patch.vertices.size());
        if (!SceneUtil::SurfaceDecals::evaluate(*source->snapshot.geometry, patch, *positions, *normals))
            return;
        std::array<std::vector<BoundVertex>, 2> halves;
        for (std::size_t i = 0; i < patch.vertices.size(); i += 3)
        {
            std::vector<BoundVertex> input;
            for (unsigned j = 0; j < 3; ++j)
            {
                const auto& anchor = patch.vertices[i + j];
                input.push_back({ { (*positions)[i + j], (*normals)[i + j], anchor.uv, osg::Vec4f(1, 1, 1, 1) },
                    anchor.indices, anchor.weights, false });
            }
            const int protectedSide = source->triangleSide(input[0].indices);
            if (protectedSide == -2) continue;
            if (protectedSide >= 0) append(halves[protectedSide], input);
            else for (unsigned side = 0; side < 2; ++side) append(halves[side], clip(input, center, normal, side));
        }
        for (unsigned side = 0; side < 2; ++side)
            if (!halves[side].empty())
            {
                auto surface = makeSurface(source, std::move(halves[side]), side, patch.stateSet, {}, true);
                surface->setNodeMask(patch.mask);
                surface->setName("Bisected NPC blood decal");
                decalTriangles += surface->vertices.size() / 3;
                (side ? upper : lower)->addChild(surface);
                surfaces.push_back(surface);
            }
    }

    void BisectionGeometry::clearDecals()
    {
        for (const auto& surface : surfaces)
            if (surface->decal)
                while (surface->getNumParents()) surface->getParent(0)->removeChild(surface);
        surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(), [](const auto& surface) { return surface->decal; }), surfaces.end());
        decalTriangles = 0;
    }

    void BisectionGeometry::prepareDrawBuffers()
    {
        for (const auto& surface : surfaces) surface->prepare();
    }
}
