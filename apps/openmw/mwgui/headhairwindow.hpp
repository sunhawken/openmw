#ifndef MWGUI_HEADHAIRWINDOW_H
#define MWGUI_HEADHAIRWINDOW_H

#include <vector>

#include <components/esm/refid.hpp>

#include "windowbase.hpp"

namespace MyGUI
{
    class ScrollBar;
    class TextBox;
}

namespace MWGui
{
    class InventoryWindow;

    /// A small box shown alongside the inventory/spells/stats/map windows that lets the player
    /// cycle through the head and hair meshes available for their own race and sex, applying the
    /// choice to the player character live (so the inventory paperdoll previews it immediately).
    class HeadHairWindow : public WindowBase
    {
    public:
        explicit HeadHairWindow(InventoryWindow* inventoryWindow);

        void onOpen() override;
        void setVisible(bool visible) override;

    private:
        void collectParts();
        void updateLabels();
        void apply();

        void onHeadScroll(MyGUI::ScrollBar* sender, size_t pos);
        void onHairScroll(MyGUI::ScrollBar* sender, size_t pos);

        InventoryWindow* mInventoryWindow;

        MyGUI::ScrollBar* mHeadSlider;
        MyGUI::ScrollBar* mHairSlider;
        MyGUI::TextBox* mHeadLabel;
        MyGUI::TextBox* mHairLabel;

        std::vector<ESM::RefId> mHeads;
        std::vector<ESM::RefId> mHairs;
        int mHeadIndex;
        int mHairIndex;
        // A change was made that still needs to be applied to the world player model; done when the
        // window hides (menu closes) so the full rebuild happens while the world update is active.
        bool mWorldModelDirty;
    };
}

#endif
