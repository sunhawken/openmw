#ifndef OPENMW_MWRENDER_VERLETCLOTHCONTROLLER_H
#define OPENMW_MWRENDER_VERLETCLOTHCONTROLLER_H

#include <components/sceneutil/nodecallback.hpp>

#include <osg/Matrix>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <vector>

namespace osg
{
    class MatrixTransform;
}

namespace MWRender
{
    /// Hair / cloth chains simulated with the method of sunhawken/physics-verlet, made 3D.
    ///
    /// Each particle keeps its position and its previous position. Gravity is the only force. Every
    /// step the distance constraints are applied several times: the first `pinCount` particles of a
    /// chain are held at their animated position (constrain_distance_from_point with distance 0),
    /// consecutive particles may not be further apart than at rest (rope.c), and neighbouring chains
    /// may not be further apart than 1.1 x their rest distance (cloth.c) so a whole sheet of chains
    /// moves together. Particles are pushed apart when they overlap (world_collide). The only
    /// additions the 2D library does not have are immovable body capsules and a floor so the
    /// cloth cannot pass through the actor.
    class VerletClothController : public SceneUtil::NodeCallback<VerletClothController, osg::MatrixTransform*>
    {
    public:
        VerletClothController(
            std::vector<std::vector<osg::ref_ptr<osg::MatrixTransform>>> chains, int pinCount, bool debug = false);

        void operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv);

    private:
        struct Chain
        {
            std::vector<osg::ref_ptr<osg::MatrixTransform>> mBones;
            std::vector<osg::Matrix> mRestLocal;
            std::vector<osg::Vec3f> mRest; // animated rest positions, world space
            std::vector<osg::Vec3f> mPos;
            std::vector<osg::Vec3f> mOld;
            std::vector<float> mSegment; // rest distance between particle i-1 and i
        };
        struct Link
        {
            std::size_t mA, mB;
            std::vector<float> mRest; // per level
        };
        struct Capsule
        {
            osg::ref_ptr<osg::MatrixTransform> mA, mB;
            float mRadius;
        };

        void initialize(osg::MatrixTransform* node);
        void computeRest(Chain& chain) const;
        void resetToRest();
        void step(float dt);
        void solveCollisions();
        void writeBones();

        std::vector<Chain> mChains;
        std::vector<Link> mLinks;
        std::vector<Capsule> mCapsules;
        osg::ref_ptr<osg::Node> mGroundNode;
        int mPinCount;
        bool mInitialized = false;
        bool mDebug;
        double mLastTime = -1.0;
        float mAccumulator = 0.f;
        float mGroundZ = 0.f;
        bool mHasGround = false;
        std::vector<osg::Vec3f> mCapA, mCapB;
    };
}

#endif
