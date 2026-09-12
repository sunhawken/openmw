#include "textinput.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_UString.h>

#include <components/settings/values.hpp>

#include <components/esm/refid.hpp>

namespace MWGui
{

    TextInputDialog::TextInputDialog()
        : WindowModal("openmw_text_input.layout")
    {
        mDisableGamepadCursor = true;
        // Centre dialog
        center();

        getWidget(mTextEdit, "TextEdit");
        mTextEdit->eventEditSelectAccept += newDelegate(this, &TextInputDialog::onTextAccepted);
        mTextEdit->eventEditTextChange += newDelegate(this, &TextInputDialog::onTextEdited);

        MyGUI::Button* okButton;
        getWidget(okButton, "OKButton");
        okButton->eventMouseButtonClick += MyGUI::newDelegate(this, &TextInputDialog::onOkClicked);
        okButton->setEnabled(false);
        okButton->setVisible(false);

        // Make sure the edit box has focus
        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mTextEdit);

        mControllerButtons = {};
        mControllerButtons.mA = "#{Interface:OK}";
    }

    void TextInputDialog::setNextButtonShow(bool shown)
    {
        MyGUI::Button* okButton;
        getWidget(okButton, "OKButton");

        if (shown)
            okButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sNext", {})));
        else
            okButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sOK", {})));
    }

    void TextInputDialog::setTextLabel(std::string_view label)
    {
        setText("LabelT", label);
    }

    void TextInputDialog::setControllerOpensKeyboard(bool enable)
    {
        mControllerOpensKeyboard = enable;
        if (mControllerOpensKeyboard)
        {
            mControllerButtons.mA = "#{Interface:ShowKeyboard}";
            mControllerButtons.mX = "#{Interface:Done}";
            mControllerButtons.mB.clear();
            mControllerButtons.mXAfterB = true;
        }
        else
        {
            mControllerButtons.mA = "#{Interface:OK}";
            mControllerButtons.mX.clear();
            mControllerButtons.mB.clear();
            mControllerButtons.mXAfterB = false;
        }
    }

    void TextInputDialog::onOpen()
    {
        WindowModal::onOpen();
        // Make sure the edit box has focus
        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mTextEdit);
        if (Settings::gui().mControllerMenus)
        {
            MWBase::Environment::get().getWindowManager()->setCursorActive(false);
            MWBase::Environment::get().getWindowManager()->setCursorVisible(false);
            MWBase::Environment::get().getInputManager()->setGamepadGuiCursorEnabled(false);
        }
    }

    // widget controls

    void TextInputDialog::onOkClicked(MyGUI::Widget* /*sender*/)
    {
        if (mTextEdit->getCaption().empty())
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage37}");
            MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mTextEdit);
        }
        else
            eventDone(this);
    }

    void TextInputDialog::onTextAccepted(MyGUI::EditBox* sender)
    {
        onOkClicked(sender);

        // To do not spam onTextAccepted() again and again
        MWBase::Environment::get().getWindowManager()->injectKeyRelease(MyGUI::KeyCode::None);
    }

    std::string TextInputDialog::getTextInput() const
    {
        return mTextEdit->getCaption();
    }

    void TextInputDialog::setTextInput(const std::string& text)
    {
        mSuppressLimitMessage = true;
        mTextEdit->setCaption(text);
        mSuppressLimitMessage = false;
        mHitLimit = mMaxTextLength > 0 && mTextEdit->getCaption().size() >= mMaxTextLength;
    }

    void TextInputDialog::setMaxTextLength(size_t length, bool showMessage)
    {
        mMaxTextLength = length;
        mShowLimitMessage = showMessage;
        mTextEdit->setMaxTextLength(length);
        mHitLimit = length > 0 && mTextEdit->getCaption().size() >= length;
    }

    void TextInputDialog::onTextEdited(MyGUI::EditBox* sender)
    {
        if (mMaxTextLength == 0 || !mShowLimitMessage)
            return;

        const size_t length = sender->getCaption().size();
        if (mSuppressLimitMessage)
        {
            mHitLimit = length >= mMaxTextLength;
            return;
        }

        if (length < mMaxTextLength)
        {
            mHitLimit = false;
            return;
        }

        if (!mHitLimit)
        {
            MWBase::Environment::get().getWindowManager()->messageBox("You've hit the character limit");
            mHitLimit = true;
        }
    }

    bool TextInputDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (mControllerOpensKeyboard)
                MWBase::Environment::get().getWindowManager()->toggleVirtualKeyboard();
            else
                onOkClicked(nullptr);
            MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_X && mControllerOpensKeyboard)
        {
            onOkClicked(nullptr);
            MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
            return true;
        }

        return false;
    }
}
