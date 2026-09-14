#include "countdialog.hpp"

#include <MyGUI_Button.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ScrollBar.h>

#include <components/widgets/numericeditbox.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

namespace MWGui
{
    CountDialog::CountDialog()
        : WindowModal("openmw_count_window.layout")
    {
        getWidget(mSlider, "CountSlider");
        getWidget(mItemEdit, "ItemEdit");
        getWidget(mItemText, "ItemText");
        getWidget(mLabelText, "LabelText");
        getWidget(mOkButton, "OkButton");
        getWidget(mCancelButton, "CancelButton");

        mCancelButton->eventMouseButtonClick += MyGUI::newDelegate(this, &CountDialog::onCancelButtonClicked);
        mOkButton->eventMouseButtonClick += MyGUI::newDelegate(this, &CountDialog::onOkButtonClicked);
        mItemEdit->eventValueChanged += MyGUI::newDelegate(this, &CountDialog::onEditValueChanged);
        mSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &CountDialog::onSliderMoved);
        // make sure we read the enter key being pressed to accept multiple items
        mItemEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &CountDialog::onEnterKeyPressed);

        mControllerButtons.mA = "#{Interface:OK}";
        mControllerButtons.mB = "#{Interface:Cancel}";
    }

    void CountDialog::openCountDialog(
        const std::string& item, const std::string& message, const int maxCount, WindowBase* tooltipSourceWindow)
    {
        setVisible(true);
        resetControllerNavHold();
        mTooltipSourceWindow = tooltipSourceWindow;

        mLabelText->setCaptionWithReplacing(message);

        MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();

        mSlider->setScrollRange(maxCount);
        mItemText->setCaptionWithReplacing(item);

        int width = std::max(mItemText->getTextSize().width + 160, 320);
        setCoord(viewSize.width / 2 - width / 2, viewSize.height / 2 - mMainWidget->getHeight() / 2, width,
            mMainWidget->getHeight());

        mSlider->setScrollPosition(maxCount - 1);

        mItemEdit->setMinValue(1);
        mItemEdit->setMaxValue(maxCount);
        mItemEdit->setValue(maxCount);
        mItemEdit->setEditStatic(true);
        mItemEdit->setNeedKeyFocus(false);
        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(nullptr);
    }

    void CountDialog::setCount(int count)
    {
        count = std::clamp(count, 1, static_cast<int>(mSlider->getScrollRange()));
        mSlider->setScrollPosition(count - 1);
        mItemEdit->setValue(count);
    }

    void CountDialog::onCancelButtonClicked(MyGUI::Widget* /*sender*/)
    {
        setVisible(false);
    }

    void CountDialog::onOkButtonClicked(MyGUI::Widget* /*sender*/)
    {
        // The order here matters. Hide the dialog first so the OK event tooltips reappear.
        setVisible(false);
        eventOkClicked(nullptr, mSlider->getScrollPosition() + 1);
    }

    // essentially duplicating what the OK button does if user presses
    // Enter key
    void CountDialog::onEnterKeyPressed(MyGUI::EditBox* sender)
    {
        onOkButtonClicked(sender);

        // To do not spam onEnterKeyPressed() again and again
        MWBase::Environment::get().getWindowManager()->injectKeyRelease(MyGUI::KeyCode::None);
    }

    void CountDialog::onEditValueChanged(int value)
    {
        mSlider->setScrollPosition(value - 1);
    }

    void CountDialog::onSliderMoved(MyGUI::ScrollBar* sender, size_t position)
    {
        mItemEdit->setValue(static_cast<int>(position + 1));
    }

    void CountDialog::resetControllerNavHold()
    {
        mNavButton = -1;
        mNavActive = false;
        mNavStartTime = {};
        mLastNavEventTime = {};
    }

    bool CountDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
            onOkButtonClicked(mOkButton);
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
            onCancelButtonClicked(mCancelButton);
        else if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
            setCount(1);
        else if (arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
            setCount(static_cast<int>(mSlider->getScrollRange()));
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            const int direction = (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? 1 : -1;
            const size_t maxCount = mSlider->getScrollRange();
            const float repeatRate = Settings::gui().mControllerMenuTurboRepeat * 7.5f;
            const auto now = std::chrono::steady_clock::now();
            float gapSeconds = 0.2f;
            if (repeatRate > 0.f)
            {
                const float intervalSeconds = 1.f / repeatRate;
                gapSeconds = std::max(gapSeconds, intervalSeconds * 1.5f);
            }
            const auto gap = std::chrono::duration<float>(gapSeconds);

            if (!mNavActive || mNavButton != static_cast<int>(arg.button) || (now - mLastNavEventTime) > gap)
            {
                mNavActive = true;
                mNavButton = static_cast<int>(arg.button);
                mNavStartTime = now;
            }

            mLastNavEventTime = now;

            int step = 1;
            if ((now - mNavStartTime) >= std::chrono::seconds(3))
            {
                if (maxCount > 1000)
                    step = 100;
                else
                    step = 10;
            }

            setCount(mItemEdit->getValue() + direction * step);
        }

        return true;
    }
}
