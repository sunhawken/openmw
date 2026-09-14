#ifndef MWGUI_CONFIRMATIONDIALOG_H
#define MWGUI_CONFIRMATIONDIALOG_H

#include "windowbase.hpp"

namespace MWGui
{
    class ConfirmationDialog : public WindowModal
    {
    public:
        ConfirmationDialog();
        void askForConfirmation(const std::string& message);
        bool exit() override;
        void setTooltipSourceWindow(WindowBase* window) { mTooltipSourceWindow = window; }
        WindowBase* getTooltipSourceWindow() const { return mTooltipSourceWindow; }

        typedef MyGUI::delegates::MultiDelegate<> EventHandle_Void;

        /** Event : Ok button was clicked.\n
            signature : void method()\n
        */
        EventHandle_Void eventOkClicked;
        EventHandle_Void eventCancelClicked;

    private:
        MyGUI::EditBox* mMessage;
        MyGUI::Button* mOkButton;
        MyGUI::Button* mCancelButton;
        MyGUI::Widget* mControllerFocusHighlight = nullptr;
        WindowBase* mTooltipSourceWindow = nullptr;

        void onCancelButtonClicked(MyGUI::Widget* sender);
        void onOkButtonClicked(MyGUI::Widget* sender);
        void updateControllerHighlight();

        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        bool mOkButtonFocus = true;
    };

}

#endif
