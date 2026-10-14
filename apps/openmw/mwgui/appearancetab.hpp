#ifndef MWGUI_APPEARANCETAB_H
#define MWGUI_APPEARANCETAB_H

#include <memory>
#include <vector>

#include <components/esm/refid.hpp>

namespace MyGUI
{
    class Button;
    class ImageBox;
    class ScrollBar;
    class TabItem;
    class TextBox;
    class Widget;
}

namespace MyGUIPlatform
{
    class OSGTexture;
}

namespace osg
{
    class Group;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWRender
{
    class JigglePreview;
}

namespace MWGui
{
    /// Options > Appearance (in-game only): cycles the player's head and hair through the meshes available for
    /// their race and sex, applied live, with a preview of the character beside the sliders.
    class AppearanceTab
    {
    public:
        AppearanceTab(MyGUI::TabItem* tab, osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem);
        ~AppearanceTab();

        MyGUI::TabItem* getTabItem() const { return mTab; }

        /// Shown: refresh the lists from the player and build the preview; hidden: drop the preview.
        void setShown(bool shown);
        void layout();
        void onFrame(float dt);

    private:
        void collectParts();
        void updateLabels();
        void apply();
        void destroyPreview();
        void updateViewCaption();

        void onHeadScroll(MyGUI::ScrollBar* sender, size_t pos);
        void onHairScroll(MyGUI::ScrollBar* sender, size_t pos);
        void onViewClicked(MyGUI::Widget* sender);

        MyGUI::TabItem* mTab;
        osg::Group* mSceneRoot;
        Resource::ResourceSystem* mResourceSystem;

        MyGUI::TextBox* mHeadLabel;
        MyGUI::TextBox* mHairLabel;
        MyGUI::ScrollBar* mHeadSlider;
        MyGUI::ScrollBar* mHairSlider;
        MyGUI::TextBox* mNote;
        MyGUI::Widget* mPreviewFrame;
        MyGUI::ImageBox* mPreviewImage;
        MyGUI::Button* mViewButton;

        std::unique_ptr<MWRender::JigglePreview> mPreview;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mPreviewTexture;
        int mView = 0; // index into the views shown on the button

        std::vector<ESM::RefId> mHeads;
        std::vector<ESM::RefId> mHairs;
        int mHeadIndex = 0;
        int mHairIndex = 0;
        bool mShown = false;
    };
}

#endif
