#ifndef MWGUI_JIGGLERETARGETPANEL_H
#define MWGUI_JIGGLERETARGETPANEL_H

#include <array>
#include <memory>
#include <vector>

#include <MyGUI_Button.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_Window.h>

#include "layout.hpp"

#include "../mwrender/jiggleautorig.hpp"

namespace osg
{
    class Group;
}
namespace Resource
{
    class ResourceSystem;
}
namespace MyGUIPlatform
{
    class OSGTexture;
}
namespace MyGUI
{
    class ITexture;
}
namespace MWRender
{
    class JigglePreview;
}

namespace MWGui
{
    /// "Retarget Jiggle": shows the front and back of the outfit the player wears and lets the user drag
    /// two breast markers (front) and two butt markers (back) to where the auto jiggle should sit. The
    /// positions are saved per mesh (see components/misc/jiggleanchors.hpp). A walking animation toggle
    /// switches the views to a live animated preview so the result can be judged in motion.
    class JiggleRetargetPanel : public Layout
    {
    public:
        JiggleRetargetPanel(osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem);
        ~JiggleRetargetPanel() override;

        bool isVisible() const;
        void setVisible(bool visible);
        void onFrame(float dt);

    private:
        static constexpr int sViewWidth = 280;
        static constexpr int sViewHeight = 340;
        static constexpr int sMarkerSize = 26;

        void open();
        void close();
        void refresh();
        void renderView(bool front, std::vector<unsigned char>& rgba) const;
        void uploadView(const char* textureName, MyGUI::ImageBox* box, const std::vector<unsigned char>& rgba);
        MyGUI::IntPoint toPixel(bool front, float y, float z) const;
        osg::Vec2f toMesh(bool front, const MyGUI::IntPoint& center) const;
        void placeMarker(std::size_t index, const osg::Vec2f& yz);
        void moveMarkerTo(std::size_t index, int absX, int absY);
        osg::Vec2f markerYZ(std::size_t index) const;
        void placeAllMarkers(bool preferAuto);
        void updateStatus();
        void setWalking(bool walking);
        void createPreviews();
        void destroyPreviews();

        void onMarkerDrag(MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton id);
        void onAutoClicked(MyGUI::Widget*);
        void onSaveClicked(MyGUI::Widget*);
        void onForgetClicked(MyGUI::Widget*);
        void onWalkClicked(MyGUI::Widget*);
        void onCloseClicked(MyGUI::Widget*);
        void applyToPlayer();

        osg::Group* mSceneRoot;
        Resource::ResourceSystem* mResourceSystem;
        MyGUI::Window* mWindow;
        MyGUI::ImageBox* mFrontView;
        MyGUI::ImageBox* mBackView;
        MyGUI::ImageBox* mFrontLive;
        MyGUI::ImageBox* mBackLive;
        MyGUI::TextBox* mStatus;
        MyGUI::Button* mWalkButton;
        std::array<MyGUI::Button*, 4> mMarkers; // breast L, breast R, butt L, butt R
        MyGUI::ITexture* mFrontTexture = nullptr;
        MyGUI::ITexture* mBackTexture = nullptr;

        std::unique_ptr<MWRender::JigglePreview> mFrontPreview;
        std::unique_ptr<MWRender::JigglePreview> mBackPreview;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mFrontPreviewTexture;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mBackPreviewTexture;

        MWRender::JiggleAutoRig::RetargetPreview mPreview;
        bool mHasPreview = false;
        bool mWalking = false;
        float mScale = 1.f;
        float mYCenter = 0.f;
        float mZCenter = 0.f;
    };
}

#endif
