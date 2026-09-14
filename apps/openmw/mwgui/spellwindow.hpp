#ifndef MWGUI_SPELLWINDOW_H
#define MWGUI_SPELLWINDOW_H

#include <memory>
#include <vector>

#include <MyGUI_Types.h>

namespace MyGUI
{
    class ImageBox;
    class Widget;
}

#include "spellicons.hpp"
#include "spellmodel.hpp"
#include "windowpinnablebase.hpp"

namespace MWGui
{
    class SpellView;

    class SpellWindow : public WindowPinnableBase, public NoDrop
    {
    public:
        SpellWindow(DragAndDrop* drag);

        void updateSpells();
        MyGUI::EditBox* getFilterEdit() const { return nullptr; }
        SpellView* getSpellView() const { return mSpellView; }
        ControllerButtons* getControllerButtons() override;

        void onFrame(float dt) override;

        /// Cycle to next/previous spell
        void cycle(bool next);

        std::string_view getWindowIdForLua() const override { return "Magic"; }
        MyGUI::Widget* getControllerFocusTooltipWidget() const;
        MyGUI::Widget* getEffectWidgetAt(const MyGUI::IntPoint& pos) const;

    protected:
        MyGUI::Widget* mEffectBox;

        ESM::RefId mSpellToDelete;

        void onEnchantedItemSelected(MWWorld::Ptr item, bool alreadyEquipped);
        void onSpellSelected(const ESM::RefId& spellId);
        void onModelIndexSelected(SpellModel::ModelIndex index);
        void onDeleteSpellAccept();
        void askDeleteSpell(const ESM::RefId& spellId);

        void onPinToggled() override;
        void onTitleDoubleClicked() override;
        void onOpen() override;
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        void setActiveControllerWindow(bool active) override;

        SpellView* mSpellView;
        std::unique_ptr<SpellIcons> mSpellIcons;
        std::vector<MyGUI::ImageBox*> mEffectIcons;
        std::vector<MyGUI::IntCoord> mEffectIconCoords;
        std::vector<MyGUI::Widget*> mEffectWidgets;
        size_t mEffectsFocus = 0;
        size_t mPrevSpellFocus = 0;
        bool mEffectsFocusActive = false;

    private:
        float mUpdateTimer;
        void resetFixedWindowGeometry();
        MyGUI::IntCoord getFixedWindowCoord(const MyGUI::IntSize& viewSize) const;
        void refreshEffectWidgets();
        void setEffectsFocusActive(bool active);
        void updateEffectsHighlight();
        bool moveEffectsFocus(int delta);
        void warpToEffectWidget();
    };
}

#endif
