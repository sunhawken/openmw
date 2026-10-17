#ifndef MWRENDER_CHARACTERPREVIEW_H
#define MWRENDER_CHARACTERPREVIEW_H

#include <memory>
#include <osg/ref_ptr>

#include <osg/PositionAttitudeTransform>

#include <components/esm3/loadnpc.hpp>

#include <components/resource/resourcesystem.hpp>

#include "../mwworld/ptr.hpp"

namespace osg
{
    class Texture2D;
    class Light;
    class Camera;
    class Group;
    class Viewport;
    class StateSet;
}

namespace MWRender
{

    class NpcAnimation;
    class DrawOnceCallback;
    class CharacterPreviewRTTNode;

    class CharacterPreview
    {
    public:
        CharacterPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character,
            int sizeX, int sizeY, const osg::Vec3f& position, const osg::Vec3f& lookAt, float lightSide = 1.f);
        virtual ~CharacterPreview();

        int getTextureWidth() const;
        int getTextureHeight() const;

        void redraw();

        void rebuild();

        /// The previewed character's third-person model, or nullptr before rebuild().
        osg::Group* getObjectRoot();

        osg::ref_ptr<osg::Texture2D> getTexture();
        /// Get the osg::StateSet required to render the texture correctly, if any.
        osg::StateSet* getTextureStateSet() { return mTextureStateSet; }

    private:
        CharacterPreview(const CharacterPreview&);
        CharacterPreview& operator=(const CharacterPreview&);

    protected:
        virtual bool renderHeadOnly() { return false; }
        void setBlendMode();
        virtual void onSetup();

        osg::ref_ptr<osg::Group> mParent;
        Resource::ResourceSystem* mResourceSystem;
        osg::ref_ptr<osg::StateSet> mTextureStateSet;
        osg::ref_ptr<DrawOnceCallback> mDrawOnceCallback;
        osg::ref_ptr<CharacterPreviewRTTNode> mRTTNode;

        /// -1 lights the subject from behind (used by the back view of the jiggle preview).
        float mLightSide;
        osg::ref_ptr<osg::Light> mLight;
        void setLightSide(float side);
        osg::Vec3f mPosition;
        osg::Vec3f mLookAt;

        MWWorld::Ptr mCharacter;

        osg::ref_ptr<MWRender::NpcAnimation> mAnimation;
        osg::ref_ptr<osg::PositionAttitudeTransform> mNode;
        std::string mCurrentAnimGroup;

        int mSizeX;
        int mSizeY;
    };

    class InventoryPreview : public CharacterPreview
    {
    public:
        InventoryPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character);

        void updatePtr(const MWWorld::Ptr& ptr);

        void update(); // Render preview again, e.g. after changed equipment
        void setViewport(int sizeX, int sizeY);

        int getSlotSelected(int posX, int posY);

    protected:
        osg::ref_ptr<osg::Viewport> mViewport;

        void onSetup() override;
    };

    /// Front or back view of a character used by the Retarget Jiggle window; it can play a walking loop in
    /// real time so the jiggle can be judged in motion.
    class JigglePreview : public CharacterPreview
    {
    public:
        enum class Motion
        {
            Still,
            Walk,
            Run,
            Bounce, // stands idle and hops in place, so the spring's overshoot and settling show clearly
        };
        enum class View
        {
            Front,
            Side,
            Back,
            Chest, // front close-up of the chest
            Face, // front close-up of the head (Options > Appearance)
        };

        JigglePreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character,
            bool front);
        void updatePtr(const MWWorld::Ptr& ptr);
        void setWalking(bool walking);
        void setMotion(Motion motion);
        Motion getMotion() const { return mMotion; }
        void setView(View view);
        View getView() const { return mView; }
        /// Redraw every frame even when still, so slider changes (gravity, offsets) show at once.
        void setAlwaysLive(bool live);
        /// Advances the animation and redraws when moving (or always live).
        void tick(float dt);

    protected:
        void onSetup() override;

    private:
        void applyPose();
        void applyView();
        bool isLive() const { return mAlwaysLive || mMotion != Motion::Still; }
        bool mFront;
        Motion mMotion = Motion::Still;
        View mView;
        bool mAlwaysLive = false;
        float mTime = 0.f;
        double mClock = 1.0;
    };

    class UpdateCameraCallback;

    class RaceSelectionPreview : public CharacterPreview
    {
        ESM::NPC mBase;
        MWWorld::LiveCellRef<ESM::NPC> mRef;

    protected:
        bool renderHeadOnly() override { return true; }
        void onSetup() override;

    public:
        RaceSelectionPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem);
        virtual ~RaceSelectionPreview();

        void setAngle(float angleRadians);

        const ESM::NPC& getPrototype() const { return mBase; }

        void setPrototype(const ESM::NPC& proto);

    private:
        osg::ref_ptr<UpdateCameraCallback> mUpdateCameraCallback;

        float mPitchRadians;
    };

}

#endif
