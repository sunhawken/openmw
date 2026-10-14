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

        // Part of the inventory group: hidden whenever Lua hides the inventory window (e.g. a mod's own
        // window opened with I.UI.setMode('Interface', { windows = {} })), since it has no Lua id of its own.
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
    };
}

#endif
