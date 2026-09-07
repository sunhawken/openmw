#ifndef OPENMW_COMPONENTS_NIFOSG_RIGGEOMETRY_H
#define OPENMW_COMPONENTS_NIFOSG_RIGGEOMETRY_H

#include <osg/Geometry>
#include <osg/Matrixf>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace SceneUtil
{
    class Skeleton;
    class Bone;

    // TODO: This class has a lot of issues.
    // - We require too many workarounds to ensure safety.
    // - mSourceGeometry should be const, but can not be const because of a use case in shadervisitor.cpp.
    // - We create useless mGeometry clones in template RigGeometries.
    // - We do not support compileGLObjects.
    // - We duplicate some code in MorphGeometry.

    /// @brief Mesh skinning implementation.
    /// @note A RigGeometry may be attached directly to a Skeleton, or somewhere below a Skeleton.
    /// Note though that the RigGeometry ignores any transforms below the Skeleton, so the attachment point is not that
    /// important.
    /// @note The internal Geometry used for rendering is double buffered, this allows updates to be done in a thread
    /// safe way while not compromising rendering performance. This is crucial when using osg's default threading model
    /// of DrawThreadPerContext.
    class RigGeometry : public osg::Drawable
    {
    public:
        RigGeometry();
        RigGeometry(const RigGeometry& copy, const osg::CopyOp& copyop);

        META_Object(SceneUtil, RigGeometry)

        // Currently empty as this is difficult to implement. Technically we would need to compile both internal
        // geometries in separate frames but this method is only called once. Alternatively we could compile just the
        // static parts of the model.
        void compileGLObjects(osg::RenderInfo& renderInfo) const override {}

        struct BoneInfo
        {
            std::string mName;
            osg::BoundingSpheref mBoundSphere;
            osg::Matrixf mInvBindMatrix;
        };

        using BoneWeight = std::pair<size_t, float>;
        using BoneWeights = std::vector<BoneWeight>;

        void setBoneInfo(std::vector<BoneInfo>&& bones);
        // Convert influences in bone and weight list per vertex format
        void setInfluences(const std::vector<BoneWeights>& influences);

        /// Initialize this geometry from the source geometry.
        /// @note The source geometry will not be modified.
        void setSourceGeometry(osg::ref_ptr<osg::Geometry> sourceGeom);

        void setTransform(osg::Matrixf&& transform);

        void setRootBone(std::string_view name);

        osg::ref_ptr<osg::Geometry> getSourceGeometry() const;

        /// Names of the bones that influence this geometry (empty if not yet initialized).
        /// Used to classify a mesh as jiggle-driven vs. rigid for the seam welder.
        std::vector<std::string> getInfluenceBoneNames() const;

        /// The bone table (name / bind / bounds) this geometry skins against. Empty if unset.
        std::vector<BoneInfo> getBoneInfoList() const;

        /// Per-vertex bone weights (indices into the bone table), expanded from the internal
        /// grouped representation. Vertices with no influence get an empty list.
        std::vector<BoneWeights> getPerVertexInfluences(std::size_t vertexCount) const;

        /// Forget the resolved skeleton/bone pointers so they are re-resolved from the current
        /// bone table on the next cull. Call after mutating the bone table / influences (e.g.
        /// the seam welder transplanting jiggle weights) on a geometry that may already be live.
        void reinitialize();

        void accept(osg::NodeVisitor& nv) override;
        bool supports(const osg::PrimitiveFunctor&) const override { return true; }
        void accept(osg::PrimitiveFunctor&) const override;

        struct CopyBoundingBoxCallback : osg::Drawable::ComputeBoundingBoxCallback
        {
            osg::BoundingBox boundingBox;

            osg::BoundingBox computeBound(const osg::Drawable&) const override { return boundingBox; }
        };

        struct CopyBoundingSphereCallback : osg::Node::ComputeBoundingSphereCallback
        {
            osg::BoundingSphere boundingSphere;

            osg::BoundingSphere computeBound(const osg::Node&) const override { return boundingSphere; }
        };

    private:
        void cull(osg::NodeVisitor* nv);
        void updateBounds(osg::NodeVisitor* nv);

        osg::ref_ptr<osg::Geometry> mGeometry[2];
        osg::Geometry* getGeometry(unsigned int frame) const;

        osg::ref_ptr<osg::Geometry> mSourceGeometry;
        osg::ref_ptr<const osg::Vec4Array> mSourceTangents;
        Skeleton* mSkeleton{ nullptr };

        osg::ref_ptr<osg::RefMatrix> mSkinToSkelMatrix;

        using VertexList = std::vector<unsigned short>;
        struct InfluenceData : public osg::Referenced
        {
            std::vector<BoneInfo> mBones;
            std::vector<std::pair<BoneWeights, VertexList>> mInfluences;
            osg::Matrixf mTransform;
            std::string mRootBone;
        };
        osg::ref_ptr<InfluenceData> mData;
        std::vector<Bone*> mNodes;

        unsigned int mLastFrameNumber{ 0 };
        bool mBoundsFirstFrame{ true };

        bool initFromParentSkeleton(osg::NodeVisitor* nv);

        void updateSkinToSkelMatrix(const osg::NodePath& nodePath);
    };

}

#endif
