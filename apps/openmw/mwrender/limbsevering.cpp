#include "limbsevering.hpp"

#include <components/debug/debuglog.hpp>
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
            int fixedCut = -1;
            BoundVertex mix(const BoundVertex& other, float t, int cut) const
            {
                return { vertex.mix(other.vertex, t), indices, weights + (other.weights - weights) * t, cut };
            }
        };

        bool finite(const osg::Vec3f& point)
        {
            return std::isfinite(point.x()) && std::isfinite(point.y()) && std::isfinite(point.z());
        }

        std::vector<BoundVertex> clip(const std::vector<BoundVertex>& polygon,
            const osg::Vec3f& center, const osg::Vec3f& normal, unsigned side, int cut)
        {
            std::vector<BoundVertex> result;
            for (std::size_t i = 0; i < polygon.size(); ++i)
            {
                const auto& a = polygon[i];
                const auto& b = polygon[(i + 1) % polygon.size()];
                const float da = (a.vertex.p - center) * normal, db = (b.vertex.p - center) * normal;
                if (side ? da >= 0 : da <= 0)
                {
                    result.push_back(a);
                    if (std::abs(da) < 1e-5f) result.back().fixedCut = cut;
                }
                if ((da < 0 && db > 0) || (da > 0 && db < 0))
                    result.push_back(a.mix(b, da / (da - db), cut));
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

    struct LimbSeveringGeometry::Source
    {
        struct BonePose { osg::Matrixf world; unsigned region = 0; };
        struct CutPose
        {
            unsigned bit;
            std::string parent, child;
            osg::Matrixf inverseParent, inverseChild;
            osg::Vec3f center, normal;
            float proximalRadius;
        };
        DecapitationGeometry::Snapshot snapshot;
        osg::ref_ptr<SceneUtil::RigGeometry> rig;
        std::vector<osg::observer_ptr<osg::Node>> sourcePath;
        BoneWorld bones;
        std::map<std::string, BonePose, Misc::StringUtils::CiComp> bonePoses;
        std::vector<CutPose> cuts;
        std::vector<std::string> anchors;
        std::vector<osg::Matrixf> initialInverse, toLocal;
        std::vector<osg::ref_ptr<osg::Geometry>> sampled;
        std::vector<unsigned> traversal;
        std::vector<unsigned> targets, distal, owners;
        std::string rigidAnchor;

        std::optional<osg::Matrixf> pose(std::string_view name, unsigned region) const
        {
            const auto found = bonePoses.find(std::string(name));
            if (found == bonePoses.end()) return bones(name);
            const auto& bone = found->second;
            if (region == 0 && bone.region != 0)
            {
                const auto& cut = cuts[bone.region - 1];
                if (const auto parent = bones(cut.parent))
                    return bone.world * cut.inverseParent * *parent;
                return {};
            }
            if (region != 0 && bone.region != region)
            {
                if (const auto anchor = bones(anchors[region]))
                    return bone.world * initialInverse[region] * *anchor;
                return {};
            }
            return bones(name);
        }

        osg::Geometry* sample(unsigned region, unsigned frame)
        {
            if (traversal[region] == frame) return sampled[region];
            traversal[region] = frame;
            sampled[region] = nullptr;
            const auto anchor = bones(anchors[region]);
            if (!anchor) return nullptr;
            toLocal[region] = osg::Matrixf::inverse(*anchor);
            if (rig)
            {
                osg::NodePath path;
                std::vector<osg::ref_ptr<osg::Node>> keepAlive;
                for (const auto& weak : sourcePath)
                {
                    osg::ref_ptr<osg::Node> node;
                    if (!weak.lock(node)) return nullptr;
                    path.push_back(node.get());
                    keepAlive.push_back(std::move(node));
                }
                const BoneWorld posed = [&](std::string_view name) { return pose(name, region); };
                sampled[region] = rig->snapshotWorldGeometry(path, posed, false, true);
            }
            else
            {
                const auto found = bonePoses.find(rigidAnchor);
                const auto current = pose(rigidAnchor, region);
                if (found == bonePoses.end() || !current) return nullptr;
                const auto correction = osg::Matrixf::inverse(found->second.world) * *current;
                sampled[region] = new osg::Geometry(*snapshot.geometry, osg::CopyOp::DEEP_COPY_ARRAYS);
                for (auto& p : *static_cast<osg::Vec3Array*>(sampled[region]->getVertexArray())) p = p * correction;
                if (auto* normals = dynamic_cast<osg::Vec3Array*>(sampled[region]->getNormalArray()))
                    for (auto& n : *normals) n = osg::Matrixf::transform3x3(n, correction);
                if (auto* tangents = dynamic_cast<osg::Vec4Array*>(sampled[region]->getTexCoordArray(7)))
                    for (auto& t : *tangents) t = osg::Vec4f(osg::Matrixf::transform3x3(
                        osg::Vec3f(t.x(), t.y(), t.z()), correction), t.w());
            }
            return sampled[region];
        }

        osg::Matrixf boundaryTransform(const BoundVertex& vertex, unsigned region) const
        {
            const auto& cut = cuts[vertex.fixedCut];
            const bool detached = region == unsigned(vertex.fixedCut + 1);
            const auto current = bones(detached ? cut.child : cut.parent);
            if (!current) return initialInverse[region];
            return (detached ? cut.inverseChild : cut.inverseParent) * *current * toLocal[region];
        }

        void partition(const std::vector<BoundVertex>& input, std::vector<std::vector<BoundVertex>>& output,
            std::vector<std::vector<CutSegment>>* segments = nullptr) const
        {
            unsigned affected = 0, protectedDistal = ~0u;
            bool entirelyBody = true;
            for (const auto& vertex : input)
                for (unsigned index : vertex.indices)
                    if (index < targets.size())
                    {
                        affected |= targets[index];
                        protectedDistal &= distal[index];
                        entirelyBody = entirelyBody && owners[index] == 1u;
                    }
            std::vector<BoundVertex> body = input;
            for (std::size_t ci = 0; ci < cuts.size() && !body.empty(); ++ci)
            {
                const auto& cut = cuts[ci];
                if (!(affected & cut.bit)) continue;
                // A deeply bent knee can put the upper thigh in front of the
                // calf plane. Only its joint neighbourhood may be clipped.
                if (entirelyBody && std::all_of(body.begin(), body.end(), [&](const auto& vertex) {
                        return (vertex.vertex.p - cut.center).length2() > cut.proximalRadius * cut.proximalRadius;
                    })) continue;
                // A folded forearm/foot is already wholly distal to this joint.
                // Do not let its world-space position cause a second plane cut.
                if (protectedDistal & cut.bit)
                {
                    append(output[ci + 1], body);
                    body.clear();
                    break;
                }
                if (segments)
                    for (std::size_t i = 1; i + 1 < body.size(); ++i)
                        appendCutSegment({ body[0].vertex.p, body[i].vertex.p, body[i + 1].vertex.p },
                            cut.center, cut.normal, (*segments)[ci]);
                append(output[ci + 1], clip(body, cut.center, cut.normal, 1, int(ci)));
                body = clip(body, cut.center, cut.normal, 0, int(ci));
            }
            append(output[0], body);
        }
    };
    class LimbSeveringGeometry::Surface final : public osg::Geometry
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
            if (lastTraversal != frame)
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
                            if (vertex.fixedCut >= 0)
                            {
                                const auto transform = source->boundaryTransform(vertex, side);
                                (*outP)[i] = vertex.vertex.p * transform;
                                (*outN)[i] = osg::Matrixf::transform3x3(vertex.vertex.n, transform);
                                (*outN)[i].normalize();
                                if (outT)
                                {
                                    const auto& tangent = vertex.vertex.tex[7];
                                    auto xyz = osg::Matrixf::transform3x3(
                                        osg::Vec3f(tangent.x(), tangent.y(), tangent.z()), transform);
                                    xyz.normalize();
                                    (*outT)[i] = osg::Vec4f(xyz, tangent.w());
                                }
                                continue;
                            }
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
        osg::ref_ptr<LimbSeveringGeometry::Surface> makeSurface(const std::shared_ptr<LimbSeveringGeometry::Source>& source,
            std::vector<BoundVertex> bound, unsigned side, osg::StateSet* state,
            const std::array<int, 8>& texDimensions = {}, bool decal = false)
        {
            std::vector<Vertex> vertices;
            vertices.reserve(bound.size());
            for (const auto& vertex : bound)
                vertices.push_back(vertex.vertex);
            const auto geometry = DecapitationGeometry::makeGeometry(vertices, state, source->initialInverse[side], texDimensions);
            osg::ref_ptr<LimbSeveringGeometry::Surface> result = new LimbSeveringGeometry::Surface;
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

    namespace
    {
        // Convex seam closure is deliberately a fallback: modular Bethesda body
        // parts may end at the joint, or leave a small open clothing seam. Such a
        // mesh still has a useful cut; it must not disable the combat outcome.
        std::vector<osg::Vec3f> projectedHull(const std::vector<osg::Vec3f>& points,
            const osg::Vec3f& center, const osg::Vec3f& u, const osg::Vec3f& v)
        {
            struct Point { osg::Vec2f xy; osg::Vec3f original; };
            std::vector<Point> ordered;
            for (const auto& p : points) ordered.push_back({ { (p - center) * u, (p - center) * v }, p });
            std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
                return a.xy.x() < b.xy.x() || (a.xy.x() == b.xy.x() && a.xy.y() < b.xy.y());
            });
            ordered.erase(std::unique(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
                return (a.xy - b.xy).length2() < 1e-8f;
            }), ordered.end());
            if (ordered.size() < 3) return {};
            const auto cross = [](const Point& a, const Point& b, const Point& c) {
                const auto x = b.xy - a.xy, y = c.xy - a.xy;
                return x.x() * y.y() - x.y() * y.x();
            };
            std::vector<Point> hull;
            for (const auto& p : ordered)
            {
                while (hull.size() > 1 && cross(hull[hull.size() - 2], hull.back(), p) <= 0) hull.pop_back();
                hull.push_back(p);
            }
            const auto lower = hull.size();
            for (auto p = ordered.rbegin() + 1; p != ordered.rend(); ++p)
            {
                while (hull.size() > lower && cross(hull[hull.size() - 2], hull.back(), *p) <= 0) hull.pop_back();
                hull.push_back(*p);
            }
            hull.pop_back();
            std::vector<osg::Vec3f> result;
            for (const auto& p : hull) result.push_back(p.original);
            return result;
        }
    }

    LimbSeveringGeometry::LimbSeveringGeometry() = default;
    LimbSeveringGeometry::~LimbSeveringGeometry() { restore(); }

    void LimbSeveringGeometry::restore()
    {
        if (committed)
            for (const auto& [node, oldMask] : originals) node->setNodeMask(oldMask);
        // Staged, hidden attachments are also removed after a failed publication.
        for (const auto& region : regions)
            while (region->getNumParents()) region->getParent(0)->removeChild(region);
        for (const auto& cut : cuts)
            for (const auto& cap : cut.caps)
                while (cap->getNumParents()) cap->getParent(0)->removeChild(cap);
        committed = false;
    }

    bool LimbSeveringGeometry::build(const std::vector<DecapitationGeometry::Snapshot>& snapshots,
        std::vector<Cut> requestedCuts, const BoneWorld& posedBones, const BoneWorld& liveBones,
        const BoneRegion& boneRegion, const BoneTargets& boneTargets, const BoneTargets& distalTargets,
        osg::StateSet* stumpCap, osg::StateSet* fragmentCap)
    {
        failure = 2;
        const auto pelvis = posedBones("Bip01 Pelvis");
        if (!pelvis || snapshots.empty() || snapshots.size() > 256 || requestedCuts.empty() || requestedCuts.size() > 4)
            return false;
        cuts = std::move(requestedCuts);
        anchors.push_back("Bip01 Pelvis");
        inverse.push_back(osg::Matrixf::inverse(*pelvis));
        for (auto& cut : cuts)
        {
            cut.caps = { new osg::Group, new osg::Group };
            cut.surfaceRadii = {};
            if (!finite(cut.center) || !finite(cut.normal) || std::abs(cut.normal.length2() - 1) > 1e-4f)
                return false;
            mask |= cut.bit;
            anchors.push_back(cut.child);
            inverse.push_back(osg::Matrixf::inverse(cut.childWorld));
            cut.childInParent = cut.childWorld * osg::Matrixf::inverse(cut.parentWorld);
        }
        for (const auto& name : anchors)
        {
            osg::ref_ptr<osg::Group> region = new osg::Group;
            region->setName("Brutal Limb Surface " + name);
            region->setCullingActive(false);
            regions.push_back(region);
        }
        std::vector<std::vector<CutSegment>> segments(cuts.size());
        std::vector<std::array<std::vector<osg::Vec3f>, 2>> nearby(cuts.size());
        std::vector<std::array<float, 2>> nearest(cuts.size(), { 1e9f, 1e9f });
        std::size_t totalVertices = 0;
        for (const auto& snapshot : snapshots)
        {
            auto source = std::make_shared<Source>();
            source->snapshot = snapshot;
            for (auto* node : snapshot.sourcePath) source->sourcePath.emplace_back(node);
            source->snapshot.sourcePath.clear();
            source->bones = liveBones;
            source->anchors = anchors;
            source->initialInverse = inverse;
            source->toLocal.resize(regions.size());
            source->sampled.resize(regions.size());
            source->traversal.resize(regions.size(), ~0u);
            for (const auto& cut : cuts)
                source->cuts.push_back({ cut.bit, cut.parent, cut.child, osg::Matrixf::inverse(cut.parentWorld),
                    osg::Matrixf::inverse(cut.childWorld), cut.center, cut.normal, std::max(8.f, cut.length * .4f) });
            source->rig = dynamic_cast<SceneUtil::RigGeometry*>(snapshot.sourceDrawable.get());
            auto* geometry = snapshot.geometry.get();
            const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray());
            const auto* normals = dynamic_cast<const osg::Vec3Array*>(geometry->getNormalArray());
            const auto* colors = dynamic_cast<const osg::Vec4Array*>(geometry->getColorArray());
            if (!positions || positions->empty() || positions->size() > 65535
                || (totalVertices += positions->size()) > 524288 || snapshot.sourcePath.empty()
                || dynamic_cast<SceneUtil::MorphGeometry*>(snapshot.sourceDrawable.get())) return false;
            source->targets.resize(positions->size());
            source->distal.resize(positions->size());
            source->owners.resize(positions->size());
            for (const auto& p : *positions) if (!finite(p)) return false;
            if (source->rig)
            {
                for (const auto& name : source->rig->getInfluencingBoneNames())
                {
                    const auto world = posedBones(name);
                    if (!world) { failure = 13; return false; }
                    source->bonePoses.emplace(name, Source::BonePose{ *world, boneRegion(name) });
                    const unsigned target = boneTargets(name), distal = distalTargets(name);
                    for (unsigned index : source->rig->getVerticesInfluencedBy(name))
                    {
                        if (index >= positions->size()) return false;
                        source->targets[index] |= target;
                        source->distal[index] |= distal;
                        source->owners[index] |= 1u << boneRegion(name);
                    }
                }
            }
            else
            {
                // Rigid armor can use a named body-part attachment below a bone.
                // Find its nearest actual skeleton bone rather than assuming pelvis.
                for (auto i = snapshot.sourcePath.rbegin(); i != snapshot.sourcePath.rend(); ++i)
                    if (!(*i)->getName().empty())
                    if (const auto world = posedBones((*i)->getName()))
                    {
                        source->rigidAnchor = (*i)->getName();
                        source->bonePoses.emplace(source->rigidAnchor,
                            Source::BonePose{ *world, boneRegion(source->rigidAnchor) });
                        break;
                    }
                if (source->rigidAnchor.empty()) { failure = 13; return false; }
                std::fill(source->targets.begin(), source->targets.end(), boneTargets(source->rigidAnchor));
                std::fill(source->distal.begin(), source->distal.end(), distalTargets(source->rigidAnchor));
                std::fill(source->owners.begin(), source->owners.end(), 1u << boneRegion(source->rigidAnchor));
            }
            for (std::size_t i = 0; i < positions->size(); ++i)
                for (std::size_t ci = 0; ci < cuts.size(); ++ci)
                    if (source->targets[i] & cuts[ci].bit)
                    {
                        const auto& cut = cuts[ci];
                        const auto delta = (*positions)[i] - cut.center;
                        const float distance = delta * cut.normal;
                        // Only the joint neighbourhood can supply an absent seam.
                        // Forearms, boots and the opposite body side cannot inflate it.
                        if (delta.length() > std::max(12.f, cut.length * .8f)) continue;
                        const unsigned side = distance > 0 ? 1 : 0;
                        nearby[ci][side].push_back((*positions)[i]);
                        nearest[ci][side] = std::min(nearest[ci][side], std::abs(distance));
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
            std::vector<std::vector<BoundVertex>> pieces(regions.size());
            for (const auto& tri : triangles.triangles)
            {
                if (++sourceTriangles > 524288 || tri[0] >= positions->size()
                    || tri[1] >= positions->size() || tri[2] >= positions->size()) return false;
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
                    input.push_back({ vertex, tri, weight, -1 });
                }
                source->partition(input, pieces, &segments);
            }
            sources.push_back(source);
            for (unsigned region = 0; region < regions.size(); ++region)
                if (!pieces[region].empty())
                {
                    auto surface = makeSurface(source, std::move(pieces[region]), region, geometry->getStateSet(), dimensions);
                    resultTriangles += surface->vertices.size() / 3;
                    articulatedSurfaces += bool(source->rig);
                    const auto* local = static_cast<const osg::Vec3Array*>(surface->getVertexArray());
                    const auto world = osg::Matrixf::inverse(inverse[region]);
                    for (std::size_t i = 0; i < local->size(); ++i)
                        reconstructionError = std::max(reconstructionError,
                            ((*local)[i] * world - surface->vertices[i].vertex.p).length());
                    osg::ref_ptr<osg::Node> material = surface;
                    for (auto state = snapshot.statePath.rbegin(); state != snapshot.statePath.rend(); ++state)
                    {
                        osg::ref_ptr<osg::Group> group = new osg::Group;
                        group->setStateSet(new osg::StateSet(**state, osg::CopyOp::DEEP_COPY_USERDATA));
                        group->addChild(material);
                        material = group;
                    }
                    regions[region]->addChild(material);
                    surfaces.push_back(surface);
                }
        }
        for (std::size_t ci = 0; ci < cuts.size(); ++ci)
        {
            auto& cut = cuts[ci];
            osg::Vec3f u = cut.normal ^ osg::Vec3f(0, 0, 1);
            if (u.length2() < .01f) u = cut.normal ^ osg::Vec3f(0, 1, 0);
            u.normalize();
            const osg::Vec3f v = cut.normal ^ u;
            std::vector<osg::Vec3f> points;
            std::vector<std::vector<int>> loops;
            std::vector<std::vector<osg::Vec3f>> rings;
            if (buildCutContours(segments[ci], points, loops) == 0)
            {
                for (const auto& loop : loops)
                {
                    std::vector<osg::Vec3f> ring;
                    for (int index : loop) ring.push_back(points[index]);
                    rings.push_back(std::move(ring));
                }
            }
            else if (!segments[ci].empty())
            {
                points.clear();
                for (const auto& [a, b] : segments[ci]) { points.push_back(a); points.push_back(b); }
                auto hull = projectedHull(points, cut.center, u, v);
                if (hull.size() >= 3) rings.push_back(std::move(hull));
                ++seamFallbacks;
            }
            for (unsigned side = 0; side < 2; ++side)
            {
                auto sideRings = rings;
                if (sideRings.empty())
                {
                    std::vector<osg::Vec3f> seam;
                    for (const auto& p : nearby[ci][side])
                        if (std::abs((p - cut.center) * cut.normal) <= nearest[ci][side] + 1.5f) seam.push_back(p);
                    auto hull = projectedHull(seam, cut.center, u, v);
                    // A modular part can be entirely on one side of the ideal
                    // plane. Use its actual rim for both cap faces in that case.
                    if (hull.size() < 3)
                    {
                        seam.clear();
                        for (const auto& p : nearby[ci][1 - side])
                            if (std::abs((p - cut.center) * cut.normal) <= nearest[ci][1 - side] + 1.5f) seam.push_back(p);
                        hull = projectedHull(seam, cut.center, u, v);
                    }
                    if (hull.size() < 3) { failure = 4; return false; }
                    sideRings.push_back(std::move(hull));
                    ++seamFallbacks;
                }
                std::vector<osg::Vec3f> exterior;
                const auto toLocal = osg::Matrixf::inverse(side ? cut.childWorld : cut.parentWorld);
                for (auto ring : sideRings)
                {
                    float area = 0, radius = 0;
                    for (std::size_t i = 0; i < ring.size(); ++i)
                    {
                        area += ((ring[i] - cut.center) ^ (ring[(i + 1) % ring.size()] - cut.center)) * cut.normal;
                        radius = std::max(radius, (ring[i] - cut.center).length());
                    }
                    if (std::abs(area) < .001f || radius > 100.f) continue;
                    if (area < 0) std::reverse(ring.begin(), ring.end());
                    osg::ref_ptr<osg::Geometry> polygon = new osg::Geometry;
                    osg::ref_ptr<osg::Vec3Array> coordinates = new osg::Vec3Array;
                    for (const auto& p : ring) coordinates->push_back(p - cut.center);
                    polygon->setVertexArray(coordinates);
                    polygon->addPrimitiveSet(new osg::DrawArrays(GL_POLYGON, 0, coordinates->size()));
                    osgUtil::Tessellator tessellator;
                    tessellator.setTessellationType(osgUtil::Tessellator::TESS_TYPE_GEOMETRY);
                    tessellator.setTessellationNormal(cut.normal);
                    tessellator.retessellatePolygons(*polygon);
                    osg::TriangleIndexFunctor<DecapitationGeometry::TriangleList> triangles;
                    polygon->accept(triangles);
                    const auto* tessellated = static_cast<const osg::Vec3Array*>(polygon->getVertexArray());
                    std::vector<Vertex> cap;
                    for (auto tri : triangles.triangles)
                    {
                        const float signedArea = (((*tessellated)[tri[1]] - (*tessellated)[tri[0]])
                            ^ ((*tessellated)[tri[2]] - (*tessellated)[tri[0]])) * cut.normal;
                        if (std::abs(signedArea) < .001f) continue;
                        if (signedArea < 0) std::swap(tri[1], tri[2]);
                        if (side) std::swap(tri[1], tri[2]);
                        for (unsigned index : tri)
                        {
                            const auto delta = (*tessellated)[index], p = delta + cut.center;
                            cap.push_back({ p, side ? -cut.normal : cut.normal,
                                osg::Vec2f(.5f + delta * u / (2 * radius), .5f + delta * v / (2 * radius)), osg::Vec4f(1, 1, 1, 1) });
                        }
                    }
                    if (cap.empty()) continue;
                    cut.caps[side]->setName("Brutal " + cut.name + (side ? " Fragment Cap" : " Stump Cap"));
                    cut.caps[side]->setCullingActive(false);
                    cut.caps[side]->addChild(DecapitationGeometry::makeGeometry(cap, side ? fragmentCap : stumpCap, toLocal));
                    capTriangles += cap.size() / 3;
                    ++capLoops;
                    exterior.insert(exterior.end(), ring.begin(), ring.end());
                }
                if (exterior.empty())
                {
                    // A closed edge graph is not necessarily a usable polygon:
                    // skin folds or coincident garment layers can cancel its area
                    // or leave the tessellator with only degenerate triangles.
                    // Close its projected outer rim directly before rejecting it.
                    const auto addFallbackCap = [&](const std::vector<osg::Vec3f>& candidates) {
                        const auto hull = projectedHull(candidates, cut.center, u, v);
                        if (hull.size() < 3) return false;
                        osg::Vec3f middle;
                        float radius = 0;
                        for (const auto& p : hull)
                        {
                            middle += p - cut.center;
                            radius = std::max(radius, (p - cut.center).length());
                        }
                        if (radius < .01f || radius > 100.f) return false;
                        middle = cut.center + middle / float(hull.size());
                        std::vector<Vertex> cap;
                        for (std::size_t i = 0; i < hull.size(); ++i)
                        {
                            std::array<osg::Vec3f, 3> triangle{ middle, hull[i], hull[(i + 1) % hull.size()] };
                            const float area = ((triangle[1] - triangle[0]) ^ (triangle[2] - triangle[0])) * cut.normal;
                            if (std::abs(area) < .001f) continue;
                            if ((area < 0) != bool(side)) std::swap(triangle[1], triangle[2]);
                            for (const auto& p : triangle)
                            {
                                const auto delta = p - cut.center;
                                cap.push_back({ p, side ? -cut.normal : cut.normal,
                                    osg::Vec2f(.5f + delta * u / (2 * radius), .5f + delta * v / (2 * radius)),
                                    osg::Vec4f(1, 1, 1, 1) });
                            }
                        }
                        if (cap.empty()) return false;
                        cut.caps[side]->setName("Brutal " + cut.name + (side ? " Fragment Cap" : " Stump Cap"));
                        cut.caps[side]->setCullingActive(false);
                        cut.caps[side]->addChild(DecapitationGeometry::makeGeometry(
                            cap, side ? fragmentCap : stumpCap, toLocal));
                        capTriangles += cap.size() / 3;
                        ++capLoops;
                        ++seamFallbacks;
                        exterior.insert(exterior.end(), hull.begin(), hull.end());
                        return true;
                    };
                    std::vector<osg::Vec3f> candidates;
                    for (const auto& ring : sideRings) candidates.insert(candidates.end(), ring.begin(), ring.end());
                    bool recovered = addFallbackCap(candidates);
                    // A negligible contour can belong to an overlapping seam
                    // sliver. In that case use the actual neighbouring joint rim.
                    for (unsigned rimSide : { side, 1 - side })
                    {
                        if (recovered) break;
                        candidates.clear();
                        for (const auto& p : nearby[ci][rimSide])
                            if (std::abs((p - cut.center) * cut.normal) <= nearest[ci][rimSide] + 1.5f)
                                candidates.push_back(p);
                        recovered = addFallbackCap(candidates);
                    }
                    if (!recovered)
                    {
                        Log(Debug::Warning) << "Limb cap preparation rejected: limb=" << cut.name << " side=" << side
                            << " segments=" << segments[ci].size() << " rings=" << sideRings.size()
                            << " nearby=" << nearby[ci][side].size() << " nearest=" << nearest[ci][side];
                        failure = 9;
                        return false;
                    }
                    Log(Debug::Verbose) << "Limb cap tessellation recovered: limb=" << cut.name << " side=" << side
                        << " segments=" << segments[ci].size() << " rings=" << sideRings.size();
                }
                // Actual modular end rims must follow the cap's joint frame too.
                // Otherwise a mixed skin weight can pull an initially closed seam
                // away from its cap when the corpse starts moving.
                for (const auto& surface : surfaces)
                    if (surface->side == (side ? ci + 1 : 0))
                        for (auto& vertex : surface->vertices)
                            if (vertex.fixedCut < 0)
                                for (const auto& p : exterior)
                                    if ((vertex.vertex.p - p).length2() < .0001f)
                                    { vertex.fixedCut = int(ci); break; }
                osg::Vec3f surfaceCenter;
                for (const auto& p : exterior) surfaceCenter += p - cut.center;
                surfaceCenter = cut.center + surfaceCenter / float(exterior.size());
                for (const auto& p : exterior)
                    cut.surfaceRadii[side] = std::max(cut.surfaceRadii[side], (p - surfaceCenter).length());
                cut.surfaceCenters[side] = surfaceCenter * toLocal;
                cut.surfaceU[side] = osg::Matrixf::transform3x3(u, toLocal);
                cut.surfaceV[side] = osg::Matrixf::transform3x3(side ? -v : v, toLocal);
            }
            if (regions[ci + 1]->getNumChildren() == 0) { failure = 10; return false; }
        }
        if (reconstructionError >= .02f) { failure = 10; return false; }
        failure = 0;
        return true;
    }

    void LimbSeveringGeometry::addDecal(osg::Drawable* drawable, const SceneUtil::SurfaceDecals::PatchData& patch)
    {
        const auto found = std::find_if(sources.begin(), sources.end(),
            [&](const auto& source) { return source->snapshot.sourceDrawable.get() == drawable; });
        if (found == sources.end() || patch.vertices.empty() || patch.vertices.size() % 3) return;
        const auto& source = *found;
        osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array(patch.vertices.size()),
            normals = new osg::Vec3Array(patch.vertices.size());
        if (!SceneUtil::SurfaceDecals::evaluate(*source->snapshot.geometry, patch, *positions, *normals)) return;
        std::vector<std::vector<BoundVertex>> pieces(regions.size());
        for (std::size_t i = 0; i < patch.vertices.size(); i += 3)
        {
            std::vector<BoundVertex> input;
            for (unsigned j = 0; j < 3; ++j)
            {
                const auto& anchor = patch.vertices[i + j];
                input.push_back({ { (*positions)[i + j], (*normals)[i + j], anchor.uv, osg::Vec4f(1, 1, 1, 1) },
                    anchor.indices, anchor.weights, -1 });
            }
            source->partition(input, pieces);
        }
        for (unsigned region = 0; region < regions.size(); ++region)
            if (!pieces[region].empty())
            {
                auto surface = makeSurface(source, std::move(pieces[region]), region, patch.stateSet, {}, true);
                surface->setNodeMask(patch.mask);
                surface->setName("Severed limb NPC blood decal");
                decalTriangles += surface->vertices.size() / 3;
                regions[region]->addChild(surface);
                surfaces.push_back(surface);
            }
    }

    void LimbSeveringGeometry::clearDecals()
    {
        for (const auto& surface : surfaces)
            if (surface->decal)
                while (surface->getNumParents()) surface->getParent(0)->removeChild(surface);
        surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(), [](const auto& surface) { return surface->decal; }), surfaces.end());
        decalTriangles = 0;
    }

    void LimbSeveringGeometry::prepareDrawBuffers()
    {
        for (const auto& surface : surfaces) surface->prepare();
    }
}
