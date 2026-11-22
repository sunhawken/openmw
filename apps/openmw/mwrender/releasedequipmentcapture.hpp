#ifndef OPENMW_MWRENDER_RELEASEDEQUIPMENTCAPTURE_H
#define OPENMW_MWRENDER_RELEASEDEQUIPMENTCAPTURE_H

#include "releasedequipment.hpp"

#include <components/esm3/releasedequipmentstate.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <osg/FrontFace>
#include <osg/Matrixd>
#include <osg/NodeVisitor>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>

namespace MWRender
{
    inline osg::NodePath releasedEquipmentPath(osg::Node* root, osg::Node* actorRoot = nullptr)
    {
        const auto paths = root->getParentalNodePaths();
        osg::NodePath result;
        osg::Matrixd selected;
        for (const auto& path : paths)
        {
            if (actorRoot && std::find(path.begin(), path.end(), actorRoot) == path.end())
                continue;
            // Match Animation::getBoneWorldMatrix: camera ancestors do not
            // become part of an item's game-world pose. Shadow/view DAG paths
            // are legitimate if they describe the same actual world transform.
            const osg::Matrixd world = osg::computeLocalToWorld(path);
            if (result.empty())
            {
                result = path;
                selected = world;
                continue;
            }
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                {
                    // Do not let a long blade amplify a tolerated difference
                    // in the linear map. Ordinary shadow/view paths share that
                    // exact map; only a tiny absolute translation is tolerated.
                    const float tolerance = row == 3 && column < 3 ? .0001f : 0.f;
                    if (!std::isfinite(world(row, column))
                        || std::abs(world(row, column) - selected(row, column)) > tolerance)
                        throw std::runtime_error("Equipped item has different transforms in actor scene paths");
                }
        }
        if (result.empty())
            throw std::runtime_error("Equipped item has no actual actor scene path");
        return result;
    }

    // Snapshot the evaluated attachment, including filtered skin geometry and
    // reflected/nonuniform transforms. Never mutate the actor or cached model.
    class ReleasedEquipmentCapture final : public osg::NodeVisitor
    {
        ESM::ReleasedEquipmentState& mOutput;
        const std::function<std::optional<osg::Matrixd>(std::string_view)>& mBones;
        osg::Vec3d mOrigin;
        osg::Matrixd mRoot, mInverse;
        osg::Node* mPartRoot;
        osg::Node* mExcluded;
        osg::Node* mActorRoot;
        std::map<std::string, uint32_t> mOrdinals;

    public:
        float maxWorldVertexError = 0.f;

        ReleasedEquipmentCapture(ESM::ReleasedEquipmentState& output, osg::Node* root, osg::Node* excluded,
            const osg::Matrixf& itemRoot,
            const std::function<std::optional<osg::Matrixd>(std::string_view)>& bones,
            osg::Node* actorRoot = nullptr)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), mOutput(output), mBones(bones),
              mOrigin(itemRoot.getTrans()), mRoot(itemRoot),
              mPartRoot(root), mExcluded(excluded), mActorRoot(actorRoot)
        {
            // Keep the exact small attachment offset before doing float skinning
            // or serializing the residual. Inverting a float exterior translation
            // first loses millimeters at Balmora/Seyda Neen and more farther out.
            mRoot.setTrans(osg::Vec3d());
            if (!mInverse.invert(mRoot))
                throw std::runtime_error("Singular released item root");
            // Respect hidden geometry. An attached arrow is excluded separately:
            // its real inventory count belongs to the ammunition slot.
            setTraversalMask(~0u);
        }

        void snapshot()
        {
            const auto path = releasedEquipmentPath(mPartRoot, mActorRoot);
            for (std::size_t i = 0; i + 1 < path.size(); ++i)
                pushOntoNodePath(path[i]);
            mPartRoot->accept(*this);
            for (std::size_t i = 0; i + 1 < path.size(); ++i)
                popFromNodePath();
            if (mOutput.mDrawables.empty())
                throw std::runtime_error("Equipped item has no visible supported geometry");
        }

        void apply(osg::Drawable& drawable) override
        {
            const auto& path = getNodePath();
            if (mExcluded && std::find(path.begin(), path.end(), mExcluded) != path.end())
                return;
            osg::ref_ptr<osg::Geometry> source, posed;
            bool world = false;
            if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
            {
                source = rig->getSourceGeometry();
                const std::function<std::optional<osg::Matrixf>(std::string_view)> relativeBones =
                    [&](std::string_view name) -> std::optional<osg::Matrixf>
                    {
                        auto matrix = mBones(name);
                        if (!matrix) return {};
                        matrix->setTrans(matrix->getTrans() - mOrigin);
                        return osg::Matrixf(*matrix);
                    };
                posed = rig->snapshotWorldGeometry(path, relativeBones, false, false, mOrigin);
                world = true;
            }
            else if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
            {
                source = morph->getSourceGeometry();
                if (!source)
                    throw std::runtime_error("Missing equipped morph source");
                posed = new osg::Geometry(*source, osg::CopyOp::DEEP_COPY_ARRAYS);
                auto* positions = dynamic_cast<osg::Vec3Array*>(posed->getVertexArray());
                const auto& targets = morph->getMorphTargetList();
                if (positions && !targets.empty())
                {
                    *positions = *targets.front().getOffsets();
                    for (std::size_t target = 1; target < targets.size(); ++target)
                    {
                        if (targets[target].getOffsets()->size() != positions->size())
                            throw std::runtime_error("Unsupported equipped morph vertex count");
                        for (std::size_t vertex = 0; vertex < positions->size(); ++vertex)
                            (*positions)[vertex] += (*targets[target].getOffsets())[vertex] * targets[target].getWeight();
                    }
                }
            }
            else
            {
                source = drawable.asGeometry();
                posed = source;
            }
            const auto* vertices = posed ? dynamic_cast<const osg::Vec3Array*>(posed->getVertexArray()) : nullptr;
            if (!source || !vertices || vertices->empty()
                || vertices->size() != source->getVertexArray()->getNumElements())
                throw std::runtime_error("Unsupported equipped item drawable");
            for (unsigned primitive = 0; primitive < source->getNumPrimitiveSets(); ++primitive)
                for (unsigned index = 0; index < source->getPrimitiveSet(primitive)->getNumIndices(); ++index)
                    if (source->getPrimitiveSet(primitive)->index(index) >= vertices->size())
                        throw std::runtime_error("Invalid equipped item topology");
            ESM::ReleasedEquipmentState::Drawable saved;
            saved.mSourceName = source->getName();
            saved.mSourceOrdinal = mOrdinals[saved.mSourceName]++;
            saved.mSourceVertexCount = vertices->size();
            saved.mSourceTopologyHash = releasedEquipmentTopologyHash(*source);
            const auto start = std::find(path.begin(), path.end(), mPartRoot);
            for (auto it = path.rbegin(); it != path.rend() && (it.base() - 1) >= start; ++it)
                if ((*it)->getUserValue("recordIndex", saved.mSourceRecordIndex))
                    break;
            for (auto it = start; it != path.end(); ++it)
                if (const auto* state = (*it)->getStateSet())
                    if (const auto* face = dynamic_cast<const osg::FrontFace*>(
                            state->getAttribute(osg::StateAttribute::FRONTFACE)))
                        saved.mClockwise = face->getMode() == osg::FrontFace::CLOCKWISE;
            osg::Matrixd drawableWorld = osg::computeLocalToWorld(path);
            drawableWorld.setTrans(drawableWorld.getTrans() - mOrigin);
            osg::Matrixd inverseWorld;
            if (!inverseWorld.invert(drawableWorld))
                throw std::runtime_error("Singular equipped drawable transform");
            const osg::Matrixf residual(drawableWorld * mInverse);
            std::copy(residual.ptr(), residual.ptr() + 16, saved.mTransform.begin());
            saved.mVertices.reserve(vertices->size());
            for (const auto& vertex : *vertices)
            {
                const osg::Vec3f local = world ? osg::Vec3d(vertex) * inverseWorld : osg::Vec3d(vertex);
                const osg::Vec3d originalWorld = world ? osg::Vec3d(vertex) : osg::Vec3d(vertex) * drawableWorld;
                // Audit the actual serialized float arrays/affine in double,
                // around a common origin. Translating both sides back to the
                // exterior cannot change this error; float world ULPs would.
                const double error = (osg::Vec3d(local) * osg::Matrixd(residual) * mRoot - originalWorld).length();
                if (!std::isfinite(error))
                    throw std::runtime_error("Non-finite equipped item continuity error");
                maxWorldVertexError = std::max(maxWorldVertexError, static_cast<float>(error));
                saved.mVertices.push_back({ local.x(), local.y(), local.z() });
            }
            // Keep the complete old affine in a residual MatrixTransform. This
            // preserves the shader's entire inverse-transposed TBN basis under
            // ordinary race scale and left-hand reflection. Rig's CPU snapshot
            // included drawableWorld linearly; undo only that instance affine.
            if (const auto* normals = dynamic_cast<const osg::Vec3Array*>(posed->getNormalArray()))
                for (const auto& value : *normals)
                {
                    const auto normal = world ? osg::Matrixd::transform3x3(value, inverseWorld) : value;
                    saved.mNormals.push_back({ normal.x(), normal.y(), normal.z() });
                }
            if (const auto* tangents = dynamic_cast<const osg::Vec4Array*>(posed->getTexCoordArray(7)))
                for (const auto& value : *tangents)
                {
                    auto tangent = osg::Vec3f(value.x(), value.y(), value.z());
                    if (world)
                        tangent = osg::Matrixd::transform3x3(tangent, inverseWorld);
                    saved.mTangents.push_back({ tangent.x(), tangent.y(), tangent.z(), value.w() });
                }
            mOutput.mDrawables.push_back(std::move(saved));
            if (mOutput.mDrawables.size() > ESM::ReleasedEquipmentState::MaxDrawables)
                throw std::runtime_error("Equipped item has too many drawables");
        }
    };
}

#endif
