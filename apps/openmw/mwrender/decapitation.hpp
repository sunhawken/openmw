#ifndef OPENMW_MWRENDER_DECAPITATION_H
#define OPENMW_MWRENDER_DECAPITATION_H

#include "decapitationcontour.hpp"
#include <components/misc/strings/algorithm.hpp>

// Procedural geometry is owned by one NpcAnimation. No model-cache object is edited.
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <osg/Material>
#include <osg/TriangleIndexFunctor>
#include <osg/observer_ptr>
#include <osgUtil/Tessellator>
#include <map>
#include <set>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>

namespace MWRender
{
    struct DecapitationGeometry
    {
        struct Snapshot
        {
            osg::ref_ptr<osg::Geometry> geometry;
            bool hair = false;
            bool helmet = false;
            std::vector<osg::ref_ptr<osg::StateSet>> statePath;
            osg::observer_ptr<osg::Drawable> sourceDrawable;
            osg::NodePath sourcePath;
        };
        struct BloodSource
        {
            // Used only to bind the stable identity immediately after build.
            // Never retains the source scene or its materials/textures.
            osg::observer_ptr<osg::Drawable> sourceDrawable;
            osg::ref_ptr<osg::Vec3Array> positions;
            int part = -1;
            unsigned ordinal = 0;
            std::string model;
            std::uint64_t fingerprint = 0;
            bool wholeHead = false;
        };
        std::vector<BloodSource> bloodSources;
        osg::Matrixf bloodNeckInverse, bloodHeadInverse;
        struct RetainedRig
        {
            osg::ref_ptr<SceneUtil::RigGeometry> geometry;
            SceneUtil::RigGeometry::BonePoseOverrides poses;
        };
        std::vector<RetainedRig> retainedRigs;
        float cutDistance=0.f, retainedVertexMaximum=-1000000.f;
        bool posedCutPlane=false;
        std::size_t retainedVertices=0, helmetInteriorTriangles=0;
        float helmetSurfaceError=0.f;
        osg::ref_ptr<osg::Group> lower = new osg::Group;
        osg::ref_ptr<osg::Group> upper = new osg::Group;
        std::vector<std::pair<osg::ref_ptr<osg::Node>, osg::Node::NodeMask>> originals;
        osg::Matrixf headInNeck;
        std::size_t sourceTriangles = 0, resultTriangles = 0, capTriangles = 0, capLoops = 0;
        float reconstructionError = 0.f;
        double preparationMilliseconds = 0.;
        bool committed = false;
        int failure = 0;
        std::vector<osg::Vec3f> diagnosticRanges;
        osg::Vec3f diagnosticCenter, diagnosticNormal;
        std::array<osg::Vec3f,2> surfaceCenters, surfaceU, surfaceV;
        float surfaceRadius=0.f;

        void restore()
        {
            if (!committed) return;
            for(auto& rig:retainedRigs)rig.geometry->clearBonePoseOverrides();
            for (auto& [node, mask] : originals) node->setNodeMask(mask);
            while (lower->getNumParents()) lower->getParent(0)->removeChild(lower);
            while (upper->getNumParents()) upper->getParent(0)->removeChild(upper);
            committed = false;
        }
        ~DecapitationGeometry() { restore(); }

        struct SnapshotVisitor : osg::NodeVisitor
        {
            std::vector<Snapshot>& output;
            const std::function<std::optional<osg::Matrixf>(std::string_view)>& bones;
            osg::Matrixf headCorrection;
            bool hair, helmet, failed = false;
            osg::Node* partRoot = nullptr;
            SnapshotVisitor(std::vector<Snapshot>& out,
                const std::function<std::optional<osg::Matrixf>(std::string_view)>& b,
                const osg::Matrixf& correction, bool isHair, bool isHelmet = false)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), output(out), bones(b),
                  headCorrection(correction), hair(isHair), helmet(isHelmet)
            { setNodeMaskOverride(~0u); }
            void snapshot(osg::Node* root)
            {
                partRoot=root;
                const auto paths=root->getParentalNodePaths();
                if(paths.empty()){failed=true;return;}
                const auto& path=paths.front();
                for(std::size_t i=0;i+1<path.size();++i)pushOntoNodePath(path[i]);
                root->accept(*this);
                for(std::size_t i=0;i+1<path.size();++i)popFromNodePath();
            }
            void apply(osg::Drawable& drawable) override
            {
                // Drawables can be shared between cached templates and many actor instances.
                // Only this visitor's prefixed traversal path identifies the requested actor.
                const osg::NodePath& path = getNodePath();
                if (path.empty()) { failed = true; return; }
                osg::ref_ptr<osg::Geometry> geometry;
                bool world = false, plainGeometry = false;
                if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                { geometry = rig->snapshotWorldGeometry(path, bones); world = true; }
                else if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                {
                    geometry = new osg::Geometry(*morph->getSourceGeometry(), osg::CopyOp::DEEP_COPY_ALL);
                    auto* v = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
                    const auto& targets = morph->getMorphTargetList();
                    if (v && !targets.empty())
                    {
                        *v = *targets.front().getOffsets();
                        for (std::size_t t=1; t<targets.size(); ++t)
                            for (std::size_t i=0; i<v->size(); ++i)
                                (*v)[i] += (*targets[t].getOffsets())[i] * targets[t].getWeight();
                    }
                }
                else if (auto* plain = drawable.asGeometry())
                {
                    geometry = new osg::Geometry(*plain, osg::CopyOp::DEEP_COPY_ALL);
                    plainGeometry=true;
                }
                if (!geometry) { failed = true; return; }
                auto* v = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
                auto* n = dynamic_cast<osg::Vec3Array*>(geometry->getNormalArray());
                if (!v || v->empty()) { failed = true; return; }
                if (!world)
                {
                    osg::Matrixf matrix = osg::computeLocalToWorld(path);
                    for (auto* node : path)
                        if (Misc::StringUtils::ciEqual(node->getName(), "Bip01 Head"))
                        { matrix *= headCorrection; break; }
                    for (auto& p : *v) p = p * matrix;
                    if (n) for (auto& p : *n) p = osg::Matrixf::transform3x3(p, matrix);
                    if(auto* tangents=dynamic_cast<osg::Vec4Array*>(geometry->getTexCoordArray(7)))
                        for(auto& t:*tangents){auto xyz=osg::Matrixf::transform3x3(osg::Vec3f(t.x(),t.y(),t.z()),matrix);t=osg::Vec4f(xyz,t.w());}
                }
                Snapshot snapshot{geometry,hair,helmet,{},&drawable,path};
                // Keep the actual local state stack, including shader AddedState metadata.
                // Flattening the entire world stack can reinterpret normal/shadow textures as diffuse maps.
                auto start=std::find(path.begin(),path.end(),partRoot);
                for(auto it=start;it!=path.end();++it)
                    if((!plainGeometry || *it!=&drawable) && (*it)->getStateSet())
                        snapshot.statePath.emplace_back(new osg::StateSet(*(*it)->getStateSet(),osg::CopyOp::DEEP_COPY_USERDATA));
                output.push_back(std::move(snapshot));
            }
        };

        struct RetainedRigVisitor : osg::NodeVisitor
        {
            std::vector<std::pair<osg::ref_ptr<SceneUtil::RigGeometry>,osg::NodePath>> rigs;
            RetainedRigVisitor():osg::NodeVisitor(TRAVERSE_ALL_CHILDREN){}
            void snapshot(osg::Node* root)
            {
                const auto paths=root->getParentalNodePaths();if(paths.empty())return;
                const auto& path=paths.front();
                for(std::size_t i=0;i+1<path.size();++i)pushOntoNodePath(path[i]);
                root->accept(*this);
                for(std::size_t i=0;i+1<path.size();++i)popFromNodePath();
            }
            void apply(osg::Drawable& drawable) override
            {
                if(auto* rig=dynamic_cast<SceneUtil::RigGeometry*>(&drawable))rigs.emplace_back(rig,getNodePath());
            }
        };

        struct TriangleList
        {
            std::vector<std::array<unsigned,3>> triangles;
            void operator()(unsigned a, unsigned b, unsigned c) { triangles.push_back({a,b,c}); }
        };
        struct Vertex
        {
            osg::Vec3f p, n;
            osg::Vec2f uv;
            osg::Vec4f color;
            std::array<osg::Vec4f,8> tex{};
            Vertex mix(const Vertex& b, float t) const
            {
                Vertex out{p+(b.p-p)*t,n+(b.n-n)*t,uv+(b.uv-uv)*t,color+(b.color-color)*t};
                for(std::size_t i=0;i<tex.size();++i)out.tex[i]=tex[i]+(b.tex[i]-tex[i])*t;
                return out;
            }
        };
        static std::vector<Vertex> clipPolygon(const std::vector<Vertex>& polygon,
            const std::function<float(const osg::Vec3f&)>& distance, bool positive)
        {
            std::vector<Vertex> result;
            for(std::size_t i=0;i<polygon.size();++i)
            {
                const auto& a=polygon[i];const auto& b=polygon[(i+1)%polygon.size()];
                const float da=distance(a.p),db=distance(b.p);
                if(positive ? da>=0.f : da<=0.f)result.push_back(a);
                if((da<0.f && db>0.f)||(da>0.f && db<0.f))result.push_back(a.mix(b,da/(da-db)));
            }
            return result;
        }
        static void appendPolygon(std::vector<Vertex>& output,const std::vector<Vertex>& polygon)
        {
            for(std::size_t i=1;i+1<polygon.size();++i)
                if(((polygon[i].p-polygon[0].p)^(polygon[i+1].p-polygon[0].p)).length2()>1e-12f)
                {output.push_back(polygon[0]);output.push_back(polygon[i]);output.push_back(polygon[i+1]);}
        }
        static osg::ref_ptr<osg::Geometry> makeGeometry(const std::vector<Vertex>& vertices,
            osg::StateSet* state, const osg::Matrixf& toLocal, const std::array<int,8>& texDimensions = {})
        {
            osg::ref_ptr<osg::Geometry> out = new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array, normals = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec2Array> uv = new osg::Vec2Array;
            osg::ref_ptr<osg::Vec4Array> color = new osg::Vec4Array;
            for (const auto& v : vertices)
            {
                positions->push_back(v.p * toLocal);
                auto n = osg::Matrixf::transform3x3(v.n, toLocal); n.normalize(); normals->push_back(n);
                uv->push_back(v.uv); color->push_back(v.color);
            }
            out->setVertexArray(positions); out->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
            out->setTexCoordArray(0,uv); out->setColorArray(color,osg::Array::BIND_PER_VERTEX);
            for(unsigned unit=0;unit<texDimensions.size();++unit)
            {
                if(!texDimensions[unit])continue;
                osg::ref_ptr<osg::Vec2Array> t2=new osg::Vec2Array;
                osg::ref_ptr<osg::Vec3Array> t3=new osg::Vec3Array;
                osg::ref_ptr<osg::Vec4Array> t4=new osg::Vec4Array;
                for(const auto& v:vertices)
                {
                    auto t=v.tex[unit];
                    if(unit==7 && texDimensions[unit]==4)
                    {auto xyz=osg::Matrixf::transform3x3(osg::Vec3f(t.x(),t.y(),t.z()),toLocal);xyz.normalize();t=osg::Vec4f(xyz,t.w());}
                    if(texDimensions[unit]==2)t2->push_back(osg::Vec2f(t.x(),t.y()));
                    else if(texDimensions[unit]==3)t3->push_back(osg::Vec3f(t.x(),t.y(),t.z()));
                    else t4->push_back(t);
                }
                if(texDimensions[unit]==2)out->setTexCoordArray(unit,t2);
                else if(texDimensions[unit]==3)out->setTexCoordArray(unit,t3);
                else out->setTexCoordArray(unit,t4);
            }
            out->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,vertices.size()));
            out->setStateSet(state); out->setUseDisplayList(false); out->setUseVertexBufferObjects(true);
            return out;
        }
        bool build(const std::vector<Snapshot>& snapshots, const osg::Matrixf& neck,
            const osg::Matrixf& head, osg::StateSet* capState, float selectedCutDistance,
            osg::Vec3f normal, osg::StateSet* headCapState = nullptr)
        {
            const auto neckInv = osg::Matrixf::inverse(neck), headInv = osg::Matrixf::inverse(head);
            headInNeck = head * neckInv;
            bloodNeckInverse = neckInv;
            bloodHeadInverse = headInv;
            bloodSources.clear();
            std::size_t bloodSourceVertices = 0;
            for (const auto& snapshot : snapshots)
            {
                const auto* positions = dynamic_cast<const osg::Vec3Array*>(snapshot.geometry->getVertexArray());
                if (!positions || !snapshot.sourceDrawable.valid() || positions->size() > 65535
                    || bloodSources.size() >= 64 || bloodSourceVertices + positions->size() > 131072)
                    continue;
                BloodSource source;
                source.sourceDrawable = snapshot.sourceDrawable;
                // A range copy has no shared VBO and keeps only the exact cut
                // pose, including the head correction used for this build.
                source.positions = new osg::Vec3Array(positions->begin(), positions->end());
                source.wholeHead = snapshot.hair;
                bloodSourceVertices += positions->size();
                bloodSources.push_back(std::move(source));
            }
            // The caller already normalized and validated this exact normal
            // against the contour oracle. Renormalizing here can change strict
            // edge crossings after float rounding; retain the accepted plane.
            if (!std::isfinite(normal.x()) || !std::isfinite(normal.y()) || !std::isfinite(normal.z())
                || std::abs(normal.length2() - 1.f) > 1e-4f)
            {failure=3;return false;}
            cutDistance=selectedCutDistance;
            const osg::Vec3f center=neck.getTrans()+normal*selectedCutDistance;
            diagnosticCenter=center;diagnosticNormal=normal;
            std::vector<std::pair<osg::Vec3f,osg::Vec3f>> segments;
            struct HelmetSurface
            {
                const Snapshot* snapshot;
                std::vector<Vertex> vertices;
                std::array<int,8> texDimensions;
            };
            std::vector<HelmetSurface> helmetSurfaces;
            std::vector<std::array<osg::Vec3f,3>> capFootprint;
            float bloodExtent=0.f;
            for (const auto& snapshot : snapshots)
            {
                auto* geom=snapshot.geometry.get();
                const auto* p=dynamic_cast<const osg::Vec3Array*>(geom->getVertexArray());
                const auto* n=dynamic_cast<const osg::Vec3Array*>(geom->getNormalArray());
                const auto* uv=dynamic_cast<const osg::Vec2Array*>(geom->getTexCoordArray(0));
                const auto* color=dynamic_cast<const osg::Vec4Array*>(geom->getColorArray());
                std::array<int,8> texDimensions{};
                for(unsigned unit=0;unit<8;++unit)
                {
                    const auto* array=geom->getTexCoordArray(unit);
                    if(array && array->getNumElements()==p->size())
                    {
                        if(dynamic_cast<const osg::Vec2Array*>(array))texDimensions[unit]=2;
                        else if(dynamic_cast<const osg::Vec3Array*>(array))texDimensions[unit]=3;
                        else if(dynamic_cast<const osg::Vec4Array*>(array))texDimensions[unit]=4;
                    }
                }
                osg::TriangleIndexFunctor<TriangleList> triangles; geom->accept(triangles);
                float minimum=std::numeric_limits<float>::max(),maximum=-minimum;
                for(const auto& vertex:*p){float d=(vertex-center)*normal;minimum=std::min(minimum,d);maximum=std::max(maximum,d);}
                diagnosticRanges.emplace_back(minimum,maximum,float(triangles.triangles.size()));
                std::vector<Vertex> halves[2];
                for (const auto& tri : triangles.triangles)
                {
                    if (tri[0]>=p->size()||tri[1]>=p->size()||tri[2]>=p->size()) return false;
                    ++sourceTriangles;
                    std::vector<Vertex> input;
                    osg::Vec3f face=((*p)[tri[1]]-(*p)[tri[0]])^((*p)[tri[2]]-(*p)[tri[0]]); face.normalize();
                    for (auto index:tri)
                    {
                        Vertex vertex{(*p)[index],n&&n->size()==p->size()?(*n)[index]:face,
                            uv&&uv->size()==p->size()?(*uv)[index]:osg::Vec2f(),
                            color&&!color->empty()?(*color)[color->size()==p->size()?index:0]:osg::Vec4f(1,1,1,1)};
                        for(unsigned unit=0;unit<8;++unit)
                        {
                            if(texDimensions[unit]==2){const auto& t=(*static_cast<const osg::Vec2Array*>(geom->getTexCoordArray(unit)))[index];vertex.tex[unit].set(t.x(),t.y(),0,0);}
                            else if(texDimensions[unit]==3){const auto& t=(*static_cast<const osg::Vec3Array*>(geom->getTexCoordArray(unit)))[index];vertex.tex[unit]=osg::Vec4f(t,0);}
                            else if(texDimensions[unit]==4)vertex.tex[unit]=(*static_cast<const osg::Vec4Array*>(geom->getTexCoordArray(unit)))[index];
                        }
                        input.push_back(vertex);
                    }
                    if (snapshot.hair) { halves[1].insert(halves[1].end(),input.begin(),input.end()); continue; }
                    appendCutSegment({input[0].p,input[1].p,input[2].p},center,normal,segments);
                    for (int side=0;side<2;++side)
                    {
                        std::vector<Vertex> poly;
                        for (int i=0;i<3;++i)
                        {
                            const auto& a=input[i]; const auto& b=input[(i+1)%3];
                            float da=(a.p-center)*normal,db=(b.p-center)*normal;
                            bool inside=side?da>=0:da<=0;
                            if (inside) poly.push_back(a);
                            if ((da<0&&db>0)||(da>0&&db<0))
                            { auto v=a.mix(b,da/(da-db));poly.push_back(v); }
                        }
                        for (std::size_t i=1;i+1<poly.size();++i)
                        { halves[side].push_back(poly[0]);halves[side].push_back(poly[i]);halves[side].push_back(poly[i+1]); }
                    }
                }
                if(snapshot.helmet)
                {
                    helmetSurfaces.push_back({&snapshot,std::move(halves[1]),texDimensions});
                    continue;
                }
                for(int side=0;side<2;++side) if(!halves[side].empty())
                {
                    auto out=makeGeometry(halves[side],geom->getStateSet(),side?headInv:neckInv,texDimensions);
                    auto* vp=static_cast<osg::Vec3Array*>(out->getVertexArray());
                    for(std::size_t i=0;i<vp->size();++i)
                        reconstructionError=std::max(reconstructionError,((*vp)[i]*(side?head:neck)-halves[side][i].p).length());
                    osg::ref_ptr<osg::Node> materialNode=out;
                    for(auto state=snapshot.statePath.rbegin();state!=snapshot.statePath.rend();++state)
                    {
                        osg::ref_ptr<osg::Group> group=new osg::Group;
                        group->setStateSet(new osg::StateSet(**state,osg::CopyOp::DEEP_COPY_USERDATA));
                        group->addChild(materialNode);materialNode=group;
                    }
                    (side?upper:lower)->addChild(materialNode);resultTriangles+=halves[side].size()/3;
                }
            }
            // Weld geometric seams, not texture indices. Reject open/nonmanifold contours rather than leave holes.
            std::vector<osg::Vec3f> points;
            std::vector<std::vector<int>> loops;
            if(const int contourFailure=buildCutContours(segments,points,loops))
            {failure=contourFailure;return false;}
            osg::Vec3f u=normal^osg::Vec3f(0,0,1);if(u.length2()<.01f)u=normal^osg::Vec3f(0,1,0);u.normalize();
            osg::Vec3f v=normal^u;
            auto xy=[&](int i){auto p=points[i]-center;return osg::Vec2f(p*u,p*v);};
            auto contains=[&](const std::vector<int>& loop,const osg::Vec2f& p){bool inside=false;
                for(std::size_t i=0,j=loop.size()-1;i<loop.size();j=i++) {auto a=xy(loop[i]),b=xy(loop[j]);
                    if((a.y()>p.y())!=(b.y()>p.y())&&p.x()<(b.x()-a.x())*(p.y()-a.y())/(b.y()-a.y())+a.x())inside=!inside;}
                return inside;};
            for(std::size_t li=0;li<loops.size();++li)
            {
                // Nested armor/skin contours share one outer blood surface, avoiding coincident cap layers.
                bool nested=false;for(std::size_t j=0;j<loops.size();++j)if(j!=li&&contains(loops[j],xy(loops[li][0])))nested=true;
                if(nested)continue;
                auto loop=loops[li];float area=0,extent=0;
                for(std::size_t i=0;i<loop.size();++i){auto a=xy(loop[i]),b=xy(loop[(i+1)%loop.size()]);area+=a.x()*b.y()-b.x()*a.y();extent=std::max(extent,a.length());}
                if(std::abs(area)<.01f||extent>40.f){failure=8;return false;}
                if(area<0)std::reverse(loop.begin(),loop.end());
                osg::ref_ptr<osg::Geometry> polygon=new osg::Geometry;
                osg::ref_ptr<osg::Vec3Array> vertices=new osg::Vec3Array;
                for(int i:loop)vertices->push_back(points[i]);
                polygon->setVertexArray(vertices);polygon->addPrimitiveSet(new osg::DrawArrays(GL_POLYGON,0,vertices->size()));
                osgUtil::Tessellator tess;tess.setTessellationType(osgUtil::Tessellator::TESS_TYPE_GEOMETRY);tess.setTessellationNormal(normal);tess.retessellatePolygons(*polygon);
                osg::TriangleIndexFunctor<TriangleList> triangles;polygon->accept(triangles);
                if(triangles.triangles.empty()){failure=9;return false;}
                bloodExtent=std::max(bloodExtent,extent);
                const auto* footprintCoordinates=static_cast<osg::Vec3Array*>(polygon->getVertexArray());
                for(const auto& tri:triangles.triangles)
                {
                    std::array<osg::Vec3f,3> footprint{(*footprintCoordinates)[tri[0]],(*footprintCoordinates)[tri[1]],(*footprintCoordinates)[tri[2]]};
                    if(((footprint[1]-footprint[0])^(footprint[2]-footprint[0]))*normal<0.f)
                        std::swap(footprint[1],footprint[2]);
                    capFootprint.push_back(footprint);
                }
                for(int side=0;side<2;++side)
                {
                    std::vector<Vertex> cap;const auto* coords=static_cast<osg::Vec3Array*>(polygon->getVertexArray());
                    for(auto tri:triangles.triangles){if(side)std::swap(tri[1],tri[2]);for(unsigned i:tri){auto p=(*coords)[i];auto d=p-center;
                        cap.push_back({p,side?-normal:normal,osg::Vec2f(.5f+d*u/(extent*2),.5f+d*v/(extent*2)),osg::Vec4f(1,1,1,1)});}}
                    (side?upper:lower)->addChild(makeGeometry(cap,side&&headCapState?headCapState:capState,side?headInv:neckInv));capTriangles+=cap.size()/3;
                }
                ++capLoops;
            }
            // Some helmets contain an opaque painted cavity floor below the neck.
            // Partition that original surface by the real cut footprint: recolor only
            // its body-facing interior, retaining every position and the exterior rim.
            for(const auto& helmet:helmetSurfaces)
            {
                std::vector<Vertex> original,blood;
                for(std::size_t i=0;i+2<helmet.vertices.size();i+=3)
                {
                    std::vector<Vertex> triangle{helmet.vertices[i],helmet.vertices[i+1],helmet.vertices[i+2]};
                    auto face=(triangle[1].p-triangle[0].p)^(triangle[2].p-triangle[0].p);face.normalize();
                    if(face*normal>=-1e-6f)
                    {appendPolygon(original,triangle);continue;}
                    const auto cutDistanceFn=[&](const osg::Vec3f& point){return (point-center)*normal;};
                    appendPolygon(original,clipPolygon(triangle,cutDistanceFn,true));
                    auto below=clipPolygon(triangle,cutDistanceFn,false);
                    std::vector<std::vector<Vertex>> remaining;
                    if(below.size()>=3)remaining.push_back(std::move(below));
                    for(const auto& footprint:capFootprint)
                    {
                        std::vector<std::vector<Vertex>> outside;
                        for(const auto& polygon:remaining)
                        {
                            auto inside=polygon;
                            for(int edge=0;edge<3 && inside.size()>=3;++edge)
                            {
                                const auto a=footprint[edge],direction=footprint[(edge+1)%3]-a;
                                const auto edgeDistance=[&](const osg::Vec3f& point){return (direction^(point-a))*normal;};
                                auto piece=clipPolygon(inside,edgeDistance,false);
                                if(piece.size()>=3)outside.push_back(std::move(piece));
                                inside=clipPolygon(inside,edgeDistance,true);
                            }
                            if(inside.size()>=3)
                            {
                                for(auto& vertex:inside)
                                {
                                    helmetSurfaceError=std::max(helmetSurfaceError,std::abs((vertex.p-triangle[0].p)*face));
                                    const auto delta=vertex.p-center;
                                    vertex.uv=osg::Vec2f(.5f+delta*u/(bloodExtent*2),.5f+delta*v/(bloodExtent*2));
                                    vertex.color=osg::Vec4f(1,1,1,1);
                                }
                                appendPolygon(blood,inside);
                            }
                        }
                        if(outside.size()>4096 || original.size()+blood.size()>262144)
                        {failure=15;return false;}
                        remaining=std::move(outside);
                        if(remaining.empty())break;
                    }
                    for(const auto& polygon:remaining)appendPolygon(original,polygon);
                }
                for(int treated=0;treated<2;++treated)
                {
                    const auto& vertices=treated?blood:original;if(vertices.empty())continue;
                    auto out=makeGeometry(vertices,treated?(headCapState?headCapState:capState):helmet.snapshot->geometry->getStateSet(),headInv,
                        treated?std::array<int,8>{}:helmet.texDimensions);
                    const auto* local=static_cast<const osg::Vec3Array*>(out->getVertexArray());
                    for(std::size_t i=0;i<local->size();++i)
                        reconstructionError=std::max(reconstructionError,((*local)[i]*head-vertices[i].p).length());
                    osg::ref_ptr<osg::Node> materialNode=out;
                    if(!treated)for(auto state=helmet.snapshot->statePath.rbegin();state!=helmet.snapshot->statePath.rend();++state)
                    {
                        osg::ref_ptr<osg::Group> group=new osg::Group;
                        group->setStateSet(new osg::StateSet(**state,osg::CopyOp::DEEP_COPY_USERDATA));
                        group->addChild(materialNode);materialNode=group;
                    }
                    upper->addChild(materialNode);resultTriangles+=vertices.size()/3;
                    if(treated)helmetInteriorTriangles+=vertices.size()/3;
                }
            }
            if(helmetSurfaceError>.01f){failure=14;return false;}
            osg::Vec3f surfaceCenter;
            for(const auto& point:points)surfaceCenter+=point;
            surfaceCenter/=float(points.size());
            for(const auto& point:points)surfaceRadius=std::max(surfaceRadius,(point-surfaceCenter).length());
            for(int side=0;side<2;++side)
            {
                const auto& inverse=side?headInv:neckInv;
                surfaceCenters[side]=surfaceCenter*inverse;
                surfaceU[side]=osg::Matrixf::transform3x3(u,inverse);
                surfaceV[side]=osg::Matrixf::transform3x3(side?-v:v,inverse);
            }
            failure=10;
            return capLoops>0&&capTriangles>0&&reconstructionError<.01f;
        }
    };
}
#endif
