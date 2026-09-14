#include "controllerbuttonsoverlay.hpp"

#include <algorithm>
#include <vector>

#include <MyGUI_RenderManager.h>
#include <MyGUI_Window.h>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/windowmanager.hpp"

namespace MWGui
{
    static constexpr ControllerButtonsOverlay::ButtonDefinition sButtonDefs[] = {
        { ControllerButtonsOverlay::Button::Button_A, "A", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_A }, &ControllerButtons::mA },
        { ControllerButtonsOverlay::Button::Button_B, "B", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_B }, &ControllerButtons::mB },
        { ControllerButtonsOverlay::Button::Button_Dpad, "Dpad", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_DPAD_UP }, &ControllerButtons::mDpad },
        { ControllerButtonsOverlay::Button::Button_L1, "L1", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_LEFTSHOULDER }, &ControllerButtons::mL1 },
        { ControllerButtonsOverlay::Button::Button_L2, "L2", ControllerButtonsOverlay::InputType_Axis,
            { .mAxis = SDL_CONTROLLER_AXIS_TRIGGERLEFT }, &ControllerButtons::mL2 },
        { ControllerButtonsOverlay::Button::Button_L3, "L3", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_LEFTSTICK }, &ControllerButtons::mL3 },
        { ControllerButtonsOverlay::Button::Button_LStick, "LStick", ControllerButtonsOverlay::InputType_Axis,
            { .mAxis = SDL_CONTROLLER_AXIS_LEFTY }, &ControllerButtons::mLStick },
        { ControllerButtonsOverlay::Button::Button_Menu, "Menu", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_BACK }, &ControllerButtons::mMenu },
        { ControllerButtonsOverlay::Button::Button_R1, "R1", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER }, &ControllerButtons::mR1 },
        { ControllerButtonsOverlay::Button::Button_R2, "R2", ControllerButtonsOverlay::InputType_Axis,
            { .mAxis = SDL_CONTROLLER_AXIS_TRIGGERRIGHT }, &ControllerButtons::mR2 },
        { ControllerButtonsOverlay::Button::Button_R3, "R3", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_RIGHTSTICK }, &ControllerButtons::mR3 },
        { ControllerButtonsOverlay::Button::Button_RStick, "RStick", ControllerButtonsOverlay::InputType_Axis,
            { .mAxis = SDL_CONTROLLER_AXIS_RIGHTY }, &ControllerButtons::mRStick },
        { ControllerButtonsOverlay::Button::Button_View, "View", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_START }, &ControllerButtons::mView },
        { ControllerButtonsOverlay::Button::Button_X, "X", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_X }, &ControllerButtons::mX },
        { ControllerButtonsOverlay::Button::Button_Y, "Y", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_Y }, &ControllerButtons::mY },
    };

    ControllerButtonsOverlay::ControllerButtonsOverlay()
        : WindowBase("openmw_controllerbuttons.layout")
    {
        MWBase::InputManager* inputMgr = MWBase::Environment::get().getInputManager();

        for (size_t i = 0; i < mButtons.size(); i++)
        {
            getWidget(mButtons[i].mImage, "Btn" + std::string(sButtonDefs[i].mName) + "Image");
            getWidget(mButtons[i].mText, "Btn" + std::string(sButtonDefs[i].mName) + "Text");
            getWidget(mButtons[i].mHBox, "Btn" + std::string(sButtonDefs[i].mName));

            if (sButtonDefs[i].mInputType == InputType_Axis)
                setIcon(mButtons[i].mImage, inputMgr->getControllerAxisIcon(sButtonDefs[i].mId.mAxis));
            else
                setIcon(mButtons[i].mImage, inputMgr->getControllerButtonIcon(sButtonDefs[i].mId.mButton));
        }

        getWidget(mHBox, "ButtonBox");
        if (mHBox)
        {
            mDefaultOrder.reserve(mHBox->getChildCount());
            for (size_t i = 0; i < mHBox->getChildCount(); ++i)
                mDefaultOrder.push_back(mHBox->getChildAt(i));
        }
    }

    int ControllerButtonsOverlay::getHeight()
    {
        MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
        return window->getHeight();
    }

    void ControllerButtonsOverlay::setAnchor(int margin)
    {
        MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
        if (!window)
            return;

        const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        const float scale = 0.85f;
        const float targetWidth = viewSize.width * scale;
        const float targetHeightFromWidth = targetWidth * 10.f / 16.f;
        const float maxHeight = viewSize.height * scale;

        int fixedWidth = static_cast<int>(targetWidth);
        int fixedHeight = static_cast<int>(targetHeightFromWidth);
        if (fixedHeight > maxHeight)
        {
            fixedHeight = static_cast<int>(maxHeight);
            fixedWidth = static_cast<int>(fixedHeight * 16.f / 10.f);
        }

        const int fixedLeft = (viewSize.width - fixedWidth) / 2;
        const int fixedTop = (viewSize.height - fixedHeight) / 2;

        const int height = window->getHeight();
        const int width = fixedWidth;
        int left = fixedLeft;
        int top = fixedTop + fixedHeight + margin;

        if (top + height > viewSize.height)
            top = std::max(0, viewSize.height - height);

        window->setCoord(left, top, width, height);
        if (mHBox)
        {
            const int horizontalPadding = 10;
            mHBox->setCoord(MyGUI::IntCoord(horizontalPadding, 0, width - (horizontalPadding * 2), height));
        }
    }

    void ControllerButtonsOverlay::setButtons(ControllerButtons* buttons)
    {
        int buttonCount = 0;
        if (buttons != nullptr)
        {
            updateButtonOrder(buttons->mXAfterB, buttons->mBLeftAlign, buttons->mXRightAlign);
            for (const auto& row : sButtonDefs)
                buttonCount += updateButton(row.mButton, buttons->*(row.mField));

            mHBox->notifyChildrenSizeChanged();
        }
        else
            updateButtonOrder(false, false, false);

        setVisible(buttonCount > 0);
    }

    void ControllerButtonsOverlay::setIcon(MyGUI::ImageBox* image, const std::string& imagePath)
    {
        if (!imagePath.empty())
            image->setImageTexture(imagePath);
    }

    int ControllerButtonsOverlay::updateButton(ControllerButtonsOverlay::Button button, const std::string& buttonStr)
    {
        if (buttonStr.empty())
        {
            mButtons[button].mHBox->setVisible(false);
            mButtons[button].mHBox->setUserString("Hidden", "true");
            return 0;
        }
        else
        {
            mButtons[button].mHBox->setVisible(true);
            mButtons[button].mHBox->setUserString("Hidden", "false");
            mButtons[button].mText->setCaptionWithReplacing(buttonStr);
            return 1;
        }
    }

    void ControllerButtonsOverlay::updateButtonOrder(bool xAfterB, bool bLeftAlign, bool xRightAlign)
    {
        if (!mHBox || mDefaultOrder.empty())
            return;

        if (mXAfterB == xAfterB && mBLeftAlign == bLeftAlign && mXRightAlign == xRightAlign)
            return;

        mXAfterB = xAfterB;
        mBLeftAlign = bLeftAlign;
        mXRightAlign = xRightAlign;
        std::vector<MyGUI::Widget*> order = mDefaultOrder;
        if (bLeftAlign)
        {
            auto aIt = std::find(order.begin(), order.end(), mButtons[Button_A].mHBox);
            auto bIt = std::find(order.begin(), order.end(), mButtons[Button_B].mHBox);
            if (aIt != order.end() && bIt != order.end() && std::next(aIt) != bIt)
            {
                MyGUI::Widget* bWidget = *bIt;
                order.erase(bIt);
                aIt = std::find(order.begin(), order.end(), mButtons[Button_A].mHBox);
                order.insert(std::next(aIt), bWidget);
            }
        }
        if (xRightAlign)
        {
            auto xIt = std::find(order.begin(), order.end(), mButtons[Button_X].mHBox);
            auto spacerIt = std::find_if(order.begin(), order.end(),
                [](MyGUI::Widget* widget) { return widget && widget->castType<Gui::Spacer>(false) != nullptr; });
            if (xIt != order.end() && spacerIt != order.end())
            {
                MyGUI::Widget* xWidget = *xIt;
                order.erase(xIt);
                spacerIt = std::find_if(order.begin(), order.end(),
                    [](MyGUI::Widget* widget) { return widget && widget->castType<Gui::Spacer>(false) != nullptr; });
                order.insert(std::next(spacerIt), xWidget);
            }
        }
        else if (xAfterB)
        {
            auto xIt = std::find(order.begin(), order.end(), mButtons[Button_X].mHBox);
            auto bIt = std::find(order.begin(), order.end(), mButtons[Button_B].mHBox);
            if (xIt != order.end() && bIt != order.end() && xIt != bIt)
            {
                MyGUI::Widget* xWidget = *xIt;
                order.erase(xIt);
                bIt = std::find(order.begin(), order.end(), mButtons[Button_B].mHBox);
                order.insert(std::next(bIt), xWidget);
            }
        }

        for (MyGUI::Widget* widget : order)
        {
            if (widget->getParent() == mHBox)
                widget->detachFromWidget();
        }
        for (MyGUI::Widget* widget : order)
            widget->attachToWidget(mHBox);
        mHBox->notifyChildrenSizeChanged();
    }
}
