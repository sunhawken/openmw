#include "mousemanager.hpp"

#include <algorithm>
#include <cmath>

#include <MyGUI_Button.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>

#include <components/sdlutil/sdlinputwrapper.hpp>
#include <components/sdlutil/sdlmappings.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwgui/mapwindow.hpp"
#include "../mwgui/settingswindow.hpp"

#include "../mwworld/player.hpp"

#include "actions.hpp"
#include "bindingsmanager.hpp"

namespace MWInput
{
    MouseManager::MouseManager(
        BindingsManager* bindingsManager, SDLUtil::InputWrapper* inputWrapper, SDL_Window* window)
        : mBindingsManager(bindingsManager)
        , mInputWrapper(inputWrapper)
        , mGuiCursorX(0)
        , mGuiCursorY(0)
        , mMouseWheel(0)
        , mMouseLookEnabled(false)
        , mGuiCursorEnabled(true)
        , mLastWarpX(-1)
        , mLastWarpY(-1)
        , mMouseMoveX(0)
        , mMouseMoveY(0)
    {
        int w, h;
        SDL_GL_GetDrawableSize(window, &w, &h);

        float uiScale = MWBase::Environment::get().getWindowManager()->getScalingFactor();
        mGuiCursorX = w / (2.f * uiScale);
        mGuiCursorY = h / (2.f * uiScale);
    }

    void MouseManager::mouseMoved(const SDLUtil::MouseMotionEvent& arg)
    {
        mBindingsManager->mouseMoved(arg);

        MWBase::InputManager* input = MWBase::Environment::get().getInputManager();
        input->setJoystickLastUsed(false);
        input->resetIdleTime();

        if (mGuiCursorEnabled)
        {
            input->setGamepadGuiCursorEnabled(true);

            // We keep track of our own mouse position, so that moving the mouse while in
            // game mode does not move the position of the GUI cursor
            MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
            float uiScale = winMgr->getScalingFactor();
            mGuiCursorX = static_cast<float>(arg.x) / uiScale;
            mGuiCursorY = static_cast<float>(arg.y) / uiScale;

            mMouseWheel = static_cast<int>(arg.z);

            MyGUI::InputManager::getInstance().injectMouseMove(
                static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), mMouseWheel);
            // FIXME: inject twice to force updating focused widget states (tooltips) resulting from changing the
            // viewport by scroll wheel
            MyGUI::InputManager::getInstance().injectMouseMove(
                static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), mMouseWheel);

            winMgr->setCursorActive(true);

            // Check if this movement is from our recent mouse warp
            bool isFromWarp = (mLastWarpX >= 0 && mLastWarpY >= 0 && std::abs(mGuiCursorX - mLastWarpX) < 0.5f
                && std::abs(mGuiCursorY - mLastWarpY) < 0.5f);

            if (Settings::gui().mControllerMenus && !winMgr->getCursorVisible()
                && (std::abs(arg.xrel) > 1 || std::abs(arg.yrel) > 1) && !isFromWarp)
            {
                // Unhide the cursor if it was hidden to show a controller tooltip.
                winMgr->setControllerTooltipVisible(false);
                winMgr->setCursorVisible(true);
            }

            // Clear warp tracking after processing
            mLastWarpX = -1;
            mLastWarpY = -1;
        }

        if (mMouseLookEnabled && !input->controlsDisabled())
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();

            const float cameraSensitivity = Settings::input().mCameraSensitivity;
            float x = arg.xrel * cameraSensitivity * (Settings::input().mInvertXAxis ? -1 : 1) / 256.f;
            float y = arg.yrel * cameraSensitivity * (Settings::input().mInvertYAxis ? -1 : 1)
                * Settings::input().mCameraYMultiplier / 256.f;

            float rot[3];
            rot[0] = -y;
            rot[1] = 0.0f;
            rot[2] = -x;

            // Only actually turn player when we're not in vanity mode
            if (!world->vanityRotateCamera(rot) && input->getControlSwitch("playerlooking"))
            {
                MWWorld::Player& player = world->getPlayer();
                player.yaw(x);
                player.pitch(y);
            }
            else if (!input->getControlSwitch("playerlooking"))
                MWBase::Environment::get().getWorld()->disableDeferredPreviewRotation();
        }
    }

    void MouseManager::mouseReleased(const SDL_MouseButtonEvent& arg, Uint8 id)
    {
        MWBase::Environment::get().getInputManager()->setJoystickLastUsed(false);

        if (mBindingsManager->isDetectingBindingState())
        {
            mBindingsManager->mouseReleased(arg, id);
        }
        else
        {
            bool guiMode = MWBase::Environment::get().getWindowManager()->isGuiMode();
            guiMode = MyGUI::InputManager::getInstance().injectMouseRelease(static_cast<int>(mGuiCursorX),
                          static_cast<int>(mGuiCursorY), SDLUtil::sdlMouseButtonToMyGui(id))
                && guiMode;

            if (mBindingsManager->isDetectingBindingState())
                return; // don't allow same mouseup to bind as initiated bind

            mBindingsManager->setPlayerControlsEnabled(!guiMode);
            mBindingsManager->mouseReleased(arg, id);
        }

        MWBase::Environment::get().getLuaManager()->inputEvent(
            { MWBase::LuaManager::InputEvent::MouseButtonReleased, arg.button });
    }

    void MouseManager::mouseWheelMoved(const SDL_MouseWheelEvent& arg)
    {
        MWBase::InputManager* input = MWBase::Environment::get().getInputManager();
        if (mBindingsManager->isDetectingBindingState() || !input->controlsDisabled())
        {
            mBindingsManager->mouseWheelMoved(arg);
        }

        input->setJoystickLastUsed(false);
        MWBase::Environment::get().getLuaManager()->inputEvent({ MWBase::LuaManager::InputEvent::MouseWheel,
            MWBase::LuaManager::InputEvent::WheelChange{ arg.x, arg.y } });
    }

    void MouseManager::mousePressed(const SDL_MouseButtonEvent& arg, Uint8 id)
    {
        MWBase::InputManager* input = MWBase::Environment::get().getInputManager();
        input->setJoystickLastUsed(false);
        bool guiMode = false;

        if (id == SDL_BUTTON_LEFT || id == SDL_BUTTON_RIGHT) // MyGUI only uses these mouse events
        {
            guiMode = MWBase::Environment::get().getWindowManager()->isGuiMode();
            guiMode = MyGUI::InputManager::getInstance().injectMousePress(static_cast<int>(mGuiCursorX),
                          static_cast<int>(mGuiCursorY), SDLUtil::sdlMouseButtonToMyGui(id))
                && guiMode;
            if (MyGUI::InputManager::getInstance().getMouseFocusWidget() != nullptr)
            {
                MyGUI::Button* b
                    = MyGUI::InputManager::getInstance().getMouseFocusWidget()->castType<MyGUI::Button>(false);
                if (b && b->getEnabled() && id == SDL_BUTTON_LEFT)
                {
                    MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
                }
            }
            MWBase::Environment::get().getWindowManager()->setCursorActive(true);
        }

        if (id == SDL_BUTTON_LEFT && MWBase::Environment::get().getWindowManager()->isConsoleMode())
        {
            MWBase::Environment::get().getWindowManager()->setConsoleSelectedObject(
                MWBase::Environment::get().getWorld()->getFocusObject());
        }

        mBindingsManager->setPlayerControlsEnabled(!guiMode);

        // Don't trigger any mouse bindings while in settings menu, otherwise rebinding controls becomes impossible
        // Also do not trigger bindings when input controls are disabled, e.g. during save loading
        if (!MWBase::Environment::get().getWindowManager()->isSettingsWindowVisible() && !input->controlsDisabled())
        {
            mBindingsManager->mousePressed(arg, id);
        }
        MWBase::Environment::get().getLuaManager()->inputEvent(
            { MWBase::LuaManager::InputEvent::MouseButtonPressed, arg.button });
    }

    void MouseManager::updateCursorMode()
    {
        bool grab = !MWBase::Environment::get().getWindowManager()->containsMode(MWGui::GM_MainMenu)
            && !MWBase::Environment::get().getWindowManager()->isConsoleMode();

        bool wasRelative = mInputWrapper->getMouseRelative();
        bool isRelative = !MWBase::Environment::get().getWindowManager()->isGuiMode();

        // don't keep the pointer away from the window edge in gui mode
        // stop using raw mouse motions and switch to system cursor movements
        mInputWrapper->setMouseRelative(isRelative);

        // we let the mouse escape in the main menu
        mInputWrapper->setGrabPointer(grab && (Settings::input().mGrabCursor || isRelative));

        // we switched to non-relative mode, move our cursor to where the in-game
        // cursor is
        if (!isRelative && wasRelative != isRelative)
        {
            warpMouse();
        }
    }

    void MouseManager::update(float dt)
    {
        SDL_GetRelativeMouseState(&mMouseMoveX, &mMouseMoveY);

        if (!mMouseLookEnabled)
            return;

        float xAxis = mBindingsManager->getActionValue(A_LookLeftRight) * 2.0f - 1.0f;
        float yAxis = mBindingsManager->getActionValue(A_LookUpDown) * 2.0f - 1.0f;
        if (xAxis == 0 && yAxis == 0)
            return;

        const float cameraSensitivity = Settings::input().mCameraSensitivity;
        const float rot[3] = {
            -yAxis * dt * 1000.0f * cameraSensitivity * (Settings::input().mInvertYAxis ? -1 : 1)
                * Settings::input().mCameraYMultiplier / 256.f,
            0.0f,
            -xAxis * dt * 1000.0f * cameraSensitivity * (Settings::input().mInvertXAxis ? -1 : 1) / 256.f,
        };

        // Only actually turn player when we're not in vanity mode
        bool playerLooking = MWBase::Environment::get().getInputManager()->getControlSwitch("playerlooking");
        if (!MWBase::Environment::get().getWorld()->vanityRotateCamera(rot) && playerLooking)
        {
            MWWorld::Player& player = MWBase::Environment::get().getWorld()->getPlayer();
            player.yaw(-rot[2]);
            player.pitch(-rot[0]);
        }
        else if (!playerLooking)
            MWBase::Environment::get().getWorld()->disableDeferredPreviewRotation();

        MWBase::Environment::get().getInputManager()->resetIdleTime();
    }

    bool MouseManager::injectMouseButtonPress(Uint8 button)
    {
        return MyGUI::InputManager::getInstance().injectMousePress(
            static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), SDLUtil::sdlMouseButtonToMyGui(button));
    }

    bool MouseManager::injectMouseButtonPressWithFocusAssist(Uint8 button, int left, int up, int right, int down)
    {
        mRestoreCursorAfterButtonRelease = false;

        if (left <= 0 && up <= 0 && right <= 0 && down <= 0)
            return injectMouseButtonPress(button);

        const MyGUI::IntSize& viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        if (viewSize.width <= 0 || viewSize.height <= 0)
            return injectMouseButtonPress(button);

        const int baseX = std::clamp(static_cast<int>(mGuiCursorX), 0, viewSize.width - 1);
        const int baseY = std::clamp(static_cast<int>(mGuiCursorY), 0, viewSize.height - 1);

        auto tryMoveAndHasFocus = [&](int x, int y) -> bool {
            MyGUI::InputManager::getInstance().injectMouseMove(x, y, mMouseWheel);
            return MyGUI::InputManager::getInstance().getMouseFocusWidget() != nullptr;
        };

        if (tryMoveAndHasFocus(baseX, baseY))
            return MyGUI::InputManager::getInstance().injectMousePress(
                baseX, baseY, SDLUtil::sdlMouseButtonToMyGui(button));

        const int clampedLeft = std::max(0, left);
        const int clampedUp = std::max(0, up);
        const int clampedRight = std::max(0, right);
        const int clampedDown = std::max(0, down);

        const int step = 2;
        int clickX = baseX;
        int clickY = baseY;

        auto tryOffset = [&](int dx, int dy) -> bool {
            if (dx < -clampedLeft || dx > clampedRight || dy < -clampedUp || dy > clampedDown)
                return false;

            const int cx = std::clamp(baseX + dx, 0, viewSize.width - 1);
            const int cy = std::clamp(baseY + dy, 0, viewSize.height - 1);
            if (!tryMoveAndHasFocus(cx, cy))
                return false;

            clickX = cx;
            clickY = cy;
            return true;
        };

        const int maxRadius = std::max(std::max(clampedLeft, clampedRight), std::max(clampedUp, clampedDown));
        bool found = false;
        for (int r = step; r <= maxRadius && !found; r += step)
        {
            for (int dx = -r; dx <= r && !found; dx += step)
                found = tryOffset(dx, -r);
            for (int dy = -r + step; dy <= r - step && !found; dy += step)
                found = tryOffset(r, dy);
            for (int dx = r; dx >= -r && !found; dx -= step)
                found = tryOffset(dx, r);
            for (int dy = r - step; dy >= -r + step && !found; dy -= step)
                found = tryOffset(-r, dy);
        }

        if (!found)
        {
            tryMoveAndHasFocus(baseX, baseY);
            return MyGUI::InputManager::getInstance().injectMousePress(
                baseX, baseY, SDLUtil::sdlMouseButtonToMyGui(button));
        }

        mRestoreCursorAfterButtonRelease = true;
        mRestoreCursorButton = button;
        mRestoreCursorX = mGuiCursorX;
        mRestoreCursorY = mGuiCursorY;
        mRestoreCursorCheckX = static_cast<float>(clickX);
        mRestoreCursorCheckY = static_cast<float>(clickY);

        mGuiCursorX = static_cast<float>(clickX);
        mGuiCursorY = static_cast<float>(clickY);
        return MyGUI::InputManager::getInstance().injectMousePress(
            clickX, clickY, SDLUtil::sdlMouseButtonToMyGui(button));
    }

    bool MouseManager::injectMouseButtonRelease(Uint8 button)
    {
        const bool released = MyGUI::InputManager::getInstance().injectMouseRelease(
            static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), SDLUtil::sdlMouseButtonToMyGui(button));

        if (mRestoreCursorAfterButtonRelease && mRestoreCursorButton == button)
        {
            const bool cursorUnchangedSincePress = std::abs(mGuiCursorX - mRestoreCursorCheckX) < 0.5f
                && std::abs(mGuiCursorY - mRestoreCursorCheckY) < 0.5f;
            if (cursorUnchangedSincePress)
            {
                mGuiCursorX = mRestoreCursorX;
                mGuiCursorY = mRestoreCursorY;
                MyGUI::InputManager::getInstance().injectMouseMove(
                    static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), mMouseWheel);
            }
            mRestoreCursorAfterButtonRelease = false;
        }

        return released;
    }

    void MouseManager::injectMouseMove(float xMove, float yMove, float mouseWheelMove)
    {
        mGuiCursorX += xMove;
        mGuiCursorY += yMove;
        const int wheelDelta = static_cast<int>(mouseWheelMove);
        mMouseWheel += wheelDelta;

        const MyGUI::IntSize& viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        mGuiCursorX = std::clamp<float>(mGuiCursorX, 0.f, static_cast<float>(viewSize.width) - 1.f);
        mGuiCursorY = std::clamp<float>(mGuiCursorY, 0.f, static_cast<float>(viewSize.height) - 1.f);

        if (Settings::gui().mControllerMenus)
        {
            MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
            if (winMgr->getCursorVisible() && winMgr->isGuiMode() && winMgr->getMode() == MWGui::GM_Inventory)
            {
                MWGui::WindowBase* activeWindow = winMgr->getActiveControllerWindow();
                auto* mapWindow = activeWindow ? dynamic_cast<MWGui::MapWindow*>(activeWindow) : nullptr;
                if (mapWindow && mapWindow->isVisible())
                {
                    const MyGUI::IntCoord rect = mapWindow->mMainWidget->getAbsoluteCoord();
                    const float minX = static_cast<float>(rect.left);
                    const float maxX = static_cast<float>(rect.left + rect.width) - 1.f;
                    const float minY = static_cast<float>(rect.top);
                    const float maxY = static_cast<float>(rect.top + rect.height) - 1.f;
                    mGuiCursorX = std::clamp<float>(mGuiCursorX, minX, maxX);
                    mGuiCursorY = std::clamp<float>(mGuiCursorY, minY, maxY);
                }
            }
        }

        MyGUI::InputManager::getInstance().injectMouseMove(
            static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), mMouseWheel);
    }

    void MouseManager::warpMouse()
    {
        if (Settings::gui().mControllerMenus)
        {
            MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
            MWBase::InputManager* inputMgr = MWBase::Environment::get().getInputManager();
            if (Settings::input().mEnableSoftwareMouse && winMgr->getCursorVisible() && inputMgr->joystickLastUsed())
            {
                MyGUI::InputManager::getInstance().injectMouseMove(
                    static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), mMouseWheel);
                return;
            }
        }

        const float uiScale = MWBase::Environment::get().getWindowManager()->getScalingFactor();
        const float windowToDrawableX = mInputWrapper->getWindowToDrawableScaleX();
        const float windowToDrawableY = mInputWrapper->getWindowToDrawableScaleY();

        const float drawableX = mGuiCursorX * uiScale;
        const float drawableY = mGuiCursorY * uiScale;

        const float windowX = windowToDrawableX > 0.f ? drawableX / windowToDrawableX : drawableX;
        const float windowY = windowToDrawableY > 0.f ? drawableY / windowToDrawableY : drawableY;

        mInputWrapper->warpMouse(static_cast<int>(std::lround(windowX)), static_cast<int>(std::lround(windowY)));
    }

    void MouseManager::warpMouseToWidget(MyGUI::Widget* widget)
    {
        float widgetX = widget->getAbsoluteCoord().left + widget->getWidth() / 2.f;
        float widgetY = widget->getAbsoluteCoord().top + widget->getHeight() / 4.f;
        if (std::abs(mGuiCursorX - widgetX) > 1 || std::abs(mGuiCursorY - widgetY) > 1)
        {
            mGuiCursorX = widgetX;
            mGuiCursorY = widgetY;
            // Remember where we warped to so we can ignore movement from this warp
            mLastWarpX = widgetX;
            mLastWarpY = widgetY;
            warpMouse();
        }
    }

}
