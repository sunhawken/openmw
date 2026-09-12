#ifndef MWGUI_VIRTUALKEYBOARD_H
#define MWGUI_VIRTUALKEYBOARD_H

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>

#include "windowbase.hpp"

namespace MWGui
{
    class VirtualKeyboard : public WindowModal
    {
    public:
        VirtualKeyboard();

        void setVisible(bool visible) override;
        void onFrame(float dt) override;
        int getHeight() const;

        void setTargetEdit(MyGUI::EditBox* edit);
        void clearTargetEdit();
        void setTooltipSourceWindow(WindowBase* window) { mTooltipSourceWindow = window; }
        WindowBase* getTooltipSourceWindow() const { return mTooltipSourceWindow; }
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        bool onControllerThumbstickEvent(const SDL_ControllerAxisEvent& arg) override;

    private:
        struct LayoutSpec
        {
            std::string mName;
            std::array<std::string, 3> mPage1Rows;
            std::array<std::string, 3> mPage1ShiftRows;
            std::array<std::string, 3> mPage2Rows;
            std::array<std::string, 3> mPage2ShiftRows;
        };

        enum class Action
        {
            Text,
            Backspace,
            Shift,
            Done,
            More,
            Space
        };

        struct KeySpec
        {
            MyGUI::Button* mButton;
            std::string mText;
            std::string mShiftText;
            Action mAction;
        };

        MyGUI::EditBox* mInputBar = nullptr;
        MyGUI::EditBox* mTargetEdit = nullptr;
        WindowBase* mTooltipSourceWindow = nullptr;
        MyGUI::Widget* mControllerHighlight = nullptr;
        std::vector<KeySpec> mKeys;
        std::unordered_map<MyGUI::Widget*, size_t> mKeyLookup;
        std::vector<std::vector<size_t>> mNavRows;
        std::vector<size_t> mTextKeys;
        std::vector<LayoutSpec> mLayouts;
        std::vector<size_t> mRowMemory;
        std::vector<bool> mRowMemoryValid;
        size_t mSpaceReturnCol = 0;
        bool mSpaceReturnValid = false;
        size_t mNextReturnCol = 0;
        bool mNextReturnValid = false;
        size_t mNavRow = 0;
        size_t mNavCol = 0;
        size_t mLayoutIndex = 0;
        size_t mPage = 0;
        bool mShifted = false;
        float mCursorBlinkTimer = 0.f;
        bool mCursorBlinkVisible = true;
        bool mUseSecondaryLayout = false;
        bool mRightTriggerPressed = false;
        size_t mKeyShift = 0;
        size_t mKeyBackspace = 0;
        size_t mKeyDone = 0;
        size_t mKeySpace = 0;
        size_t mKeyMore = 0;
        std::string mCaptionShift;
        std::string mCaptionBackspace;
        std::string mCaptionDone;
        std::string mCaptionSpace;
        std::string mCaptionNext;
        std::string mCaptionPrev;

        size_t registerTextKey(std::string_view widgetName, std::string_view text, std::string_view shifted);
        size_t registerActionKey(std::string_view widgetName, Action action, std::string_view caption);
        void onKeyClicked(MyGUI::Widget* sender);
        void updateKeyCaptions();
        void updateActionCaptions();
        void toggleMorePage();
        void updateControllerButtons();
        void reloadLayout();
        void toggleKeyboardLayout();
        bool hasSecondaryLayout() const;
        void applyLayout();
        void ensureRowsVisible();
        void centerInMenuArea();
        void syncInputBar();
        void injectText(std::string_view text);
        void injectBackspace();
        void selectNav(size_t row, size_t col);
        void updateControllerHighlight();
        size_t mapColumn(size_t fromRow, size_t toRow) const;
        bool isNavRowUsable(size_t row) const;
        size_t findNextNavRow(size_t startRow, int delta) const;
    };
}

#endif
