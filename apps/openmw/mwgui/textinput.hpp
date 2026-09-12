#ifndef MWGUI_TEXT_INPUT_H
#define MWGUI_TEXT_INPUT_H

#include <cstddef>

#include "windowbase.hpp"

namespace MWGui
{
    class TextInputDialog : public WindowModal
    {
    public:
        TextInputDialog();

        std::string getTextInput() const;
        void setTextInput(const std::string& text);
        void setMaxTextLength(size_t length, bool showMessage);

        void setNextButtonShow(bool shown);
        void setTextLabel(std::string_view label);
        void setControllerOpensKeyboard(bool enable);
        void onOpen() override;

        bool exit() override { return false; }
        MyGUI::EditBox* getEditBox() const { return mTextEdit; }

        /** Event : Dialog finished, OK button clicked.\n
            signature : void method()\n
        */
        EventHandle_WindowBase eventDone;

    protected:
        void onOkClicked(MyGUI::Widget* sender);
        void onTextAccepted(MyGUI::EditBox* sender);
        void onTextEdited(MyGUI::EditBox* sender);
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;

    private:
        MyGUI::EditBox* mTextEdit;
        bool mControllerOpensKeyboard = false;
        size_t mMaxTextLength = 0;
        bool mShowLimitMessage = false;
        bool mHitLimit = false;
        bool mSuppressLimitMessage = false;
    };
}
#endif
