#include "confirmationdialog.hpp"

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_Widget.h>

#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

namespace MWGui
{
    ConfirmationDialog::ConfirmationDialog()
        : WindowModal("openmw_confirmation_dialog.layout")
    {
        getWidget(mMessage, "Message");
        getWidget(mOkButton, "OkButton");
        getWidget(mCancelButton, "CancelButton");

        mCancelButton->eventMouseButtonClick += MyGUI::newDelegate(this, &ConfirmationDialog::onCancelButtonClicked);
        mOkButton->eventMouseButtonClick += MyGUI::newDelegate(this, &ConfirmationDialog::onOkButtonClicked);

        if (Settings::gui().mControllerMenus)
        {
            mDisableGamepadCursor = true;
            mControllerButtons.mA = "#{Interface:Select}";
            mControllerButtons.mB.clear();

            MyGUI::Widget* highlightParent = mMainWidget->getClientWidget();
            if (!highlightParent)
                highlightParent = mMainWidget;
            mControllerFocusHighlight = highlightParent->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerFocusHighlight->setNeedMouseFocus(false);
            mControllerFocusHighlight->setDepth(1);
            mControllerFocusHighlight->setVisible(false);
        }
    }

    void ConfirmationDialog::askForConfirmation(const std::string& message)
    {
        setVisible(true);
        mTooltipSourceWindow = nullptr;

        mMessage->setCaptionWithReplacing(message);

        int height = mMessage->getTextSize().height + 60;

        int width = mMessage->getTextSize().width + 24;

        mMainWidget->setSize(width, height);

        mMessage->setSize(mMessage->getWidth(), mMessage->getTextSize().height + 24);

        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mOkButton);

        if (Settings::gui().mControllerMenus)
        {
            mOkButtonFocus = true;
            mOkButton->setStateSelected(true);
            mCancelButton->setStateSelected(false);
            updateControllerHighlight();
        }

        center();
    }

    bool ConfirmationDialog::exit()
    {
        setVisible(false);
        mTooltipSourceWindow = nullptr;
        if (mControllerFocusHighlight)
            mControllerFocusHighlight->setVisible(false);
        eventCancelClicked();
        return true;
    }

    void ConfirmationDialog::onCancelButtonClicked(MyGUI::Widget* /*sender*/)
    {
        exit();
    }

    void ConfirmationDialog::onOkButtonClicked(MyGUI::Widget* /*sender*/)
    {
        setVisible(false);
        mTooltipSourceWindow = nullptr;

        eventOkClicked();
    }

    bool ConfirmationDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (mOkButtonFocus)
                onOkButtonClicked(mOkButton);
            else
                onCancelButtonClicked(mCancelButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onCancelButtonClicked(mCancelButton);
        }
        else if ((arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT && !mOkButtonFocus)
            || (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && mOkButtonFocus))
        {
            mOkButtonFocus = !mOkButtonFocus;
            mOkButton->setStateSelected(mOkButtonFocus);
            mCancelButton->setStateSelected(!mOkButtonFocus);
            updateControllerHighlight();
        }

        return true;
    }

    void ConfirmationDialog::updateControllerHighlight()
    {
        if (!mControllerFocusHighlight || !Settings::gui().mControllerMenus)
            return;

        MyGUI::Widget* focus = mOkButtonFocus ? mOkButton : mCancelButton;
        if (!focus)
        {
            mControllerFocusHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* highlightParent = mControllerFocusHighlight->getParent();
        const MyGUI::IntCoord baseCoord
            = highlightParent ? highlightParent->getAbsoluteCoord() : mMainWidget->getAbsoluteCoord();
        const MyGUI::IntCoord focusCoord = focus->getAbsoluteCoord();
        mControllerFocusHighlight->setCoord(
            focusCoord.left - baseCoord.left, focusCoord.top - baseCoord.top, focusCoord.width, focusCoord.height);
        mControllerFocusHighlight->setVisible(true);
    }
}
