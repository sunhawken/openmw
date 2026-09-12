#ifndef MWGUI_COUNTDIALOG_H
#define MWGUI_COUNTDIALOG_H

#include <chrono>

#include "windowbase.hpp"

namespace Gui
{
    class NumericEditBox;
}

namespace MWGui
{
    class CountDialog : public WindowModal
    {
    public:
        CountDialog();
        void openCountDialog(const std::string& item, const std::string& message, const int maxCount,
            WindowBase* tooltipSourceWindow = nullptr);
        void setCount(int count);
        WindowBase* getTooltipSourceWindow() const { return mTooltipSourceWindow; }

        /** Event : Ok button was clicked.\n
            signature : void method(MyGUI::Widget* sender, std::size_t count)\n
        */
        MyGUI::delegates::MultiDelegate<MyGUI::Widget*, std::size_t> eventOkClicked;

    private:
        MyGUI::ScrollBar* mSlider;
        Gui::NumericEditBox* mItemEdit;
        MyGUI::TextBox* mItemText;
        MyGUI::TextBox* mLabelText;
        MyGUI::Button* mOkButton;
        MyGUI::Button* mCancelButton;
        WindowBase* mTooltipSourceWindow = nullptr;
        int mNavButton = -1;
        bool mNavActive = false;
        std::chrono::steady_clock::time_point mNavStartTime{};
        std::chrono::steady_clock::time_point mLastNavEventTime{};

        void onCancelButtonClicked(MyGUI::Widget* sender);
        void onOkButtonClicked(MyGUI::Widget* sender);
        void onEditValueChanged(int value);
        void onSliderMoved(MyGUI::ScrollBar* sender, size_t position);
        void onEnterKeyPressed(MyGUI::EditBox* sender);
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        void resetControllerNavHold();
    };

}

#endif
