#include "controllermanager.hpp"

#include <cmath>

#include <MyGUI_Button.h>
#include <MyGUI_InputManager.h>

#include <SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/files/conversion.hpp>
#include <components/sdlutil/sdlmappings.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwgui/bookwindow.hpp"
#include "../mwgui/journalwindow.hpp"
#include "../mwgui/mapwindow.hpp"
#include "../mwgui/race.hpp"
#include "../mwgui/windowbase.hpp"

#include "actions.hpp"
#include "bindingsmanager.hpp"
#include "mousemanager.hpp"

namespace
{
    constexpr float kLeftStickAxisMax = 32768.0f;
    constexpr float kGuiJoystickDeadzone = 0.15f;
    constexpr float kLeftStickDpadDeadzone = 0.85f;
    constexpr float kLeftStickDpadReleaseDeadzone = 0.85f;
    constexpr int kLeftStickDpadNone = -1;
    constexpr float kGuiTurboRepeatStartDelay = 0.25f;
    constexpr int kTriggerPressThreshold = 16000;
    constexpr int kTriggerReleaseThreshold = 8000;
    constexpr int kDialogueTriggerScrollTicks = 25;
    constexpr int kDialogueTriggerImmediateTicks = 3;
    constexpr float kDialogueTriggerScrollSmoothRate = 120.f;

    void applyRadialDeadzone(float xIn, float yIn, float deadzone, float& xOut, float& yOut)
    {
        if (deadzone <= 0.f)
        {
            xOut = xIn;
            yOut = yIn;
            return;
        }

        const float magnitude = std::sqrt(xIn * xIn + yIn * yIn);
        if (magnitude <= deadzone)
        {
            xOut = 0.f;
            yOut = 0.f;
            return;
        }

        const float scaled = (magnitude - deadzone) / (1.f - deadzone);
        xOut = (xIn / magnitude) * scaled;
        yOut = (yIn / magnitude) * scaled;
    }

    int pickLeftStickDpadButton(int x, int y, float deadzone)
    {
        float xAxis = static_cast<float>(x) / kLeftStickAxisMax;
        float yAxis = static_cast<float>(y) / kLeftStickAxisMax;
        applyRadialDeadzone(xAxis, yAxis, deadzone, xAxis, yAxis);
        if (xAxis == 0.f && yAxis == 0.f)
            return kLeftStickDpadNone;

        if (std::abs(xAxis) >= std::abs(yAxis))
            return xAxis > 0.f ? SDL_CONTROLLER_BUTTON_DPAD_RIGHT : SDL_CONTROLLER_BUTTON_DPAD_LEFT;

        return yAxis > 0.f ? SDL_CONTROLLER_BUTTON_DPAD_DOWN : SDL_CONTROLLER_BUTTON_DPAD_UP;
    }

    bool isDpadButton(int button)
    {
        return button == SDL_CONTROLLER_BUTTON_DPAD_UP || button == SDL_CONTROLLER_BUTTON_DPAD_DOWN
            || button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    }

    MyGUI::KeyCode dpadButtonToKey(int button)
    {
        switch (button)
        {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                return MyGUI::KeyCode::ArrowUp;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                return MyGUI::KeyCode::ArrowDown;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                return MyGUI::KeyCode::ArrowLeft;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                return MyGUI::KeyCode::ArrowRight;
            default:
                return MyGUI::KeyCode::None;
        }
    }

    bool isCharGenMouseAllowed(MWGui::GuiMode mode)
    {
        return mode == MWGui::GM_Name;
    }

    bool isDialogueMode(MWGui::GuiMode mode)
    {
        return mode == MWGui::GM_Dialogue;
    }

    bool isInventoryMode(MWGui::GuiMode mode)
    {
        return mode == MWGui::GM_Inventory || mode == MWGui::GM_Container || mode == MWGui::GM_Companion
            || mode == MWGui::GM_Barter;
    }

    bool usesTriggerScroll(MWGui::WindowBase* window, MWBase::WindowManager* winMgr)
    {
        if (!window || !window->isVisible())
            return false;
        if (!winMgr->isGuiMode() || winMgr->isSettingsWindowVisible())
            return false;
        if (window->getControllerScrollWidget() == nullptr)
            return false;

        MWGui::ControllerButtons* buttons = window->getControllerButtons();
        if (!buttons)
            return false;
        return !buttons->mL2.empty() || !buttons->mR2.empty();
    }

    bool usesTriggerPaging(MWGui::WindowBase* window)
    {
        if (!window || !window->isVisible())
            return false;
        return dynamic_cast<MWGui::BookWindow*>(window) != nullptr
            || dynamic_cast<MWGui::JournalWindow*>(window) != nullptr;
    }

    enum class ControllerIconStyle
    {
        Auto,
        Steam,
        Xbox,
        Playstation,
        Switch,
        Gamecube,
    };

    ControllerIconStyle getControllerIconStyle()
    {
        const std::string& style = Settings::input().mControllerIconStyle;
        if (style == "steam")
            return ControllerIconStyle::Steam;
        if (style == "xbox")
            return ControllerIconStyle::Xbox;
        if (style == "playstation")
            return ControllerIconStyle::Playstation;
        if (style == "switch")
            return ControllerIconStyle::Switch;
        if (style == "gamecube")
            return ControllerIconStyle::Gamecube;
        return ControllerIconStyle::Auto;
    }
}

namespace MWInput
{
    ControllerManager::ControllerManager(BindingsManager* bindingsManager, MouseManager* mouseManager,
        const std::filesystem::path& userControllerBindingsFile, const std::filesystem::path& controllerBindingsFile)
        : mBindingsManager(bindingsManager)
        , mMouseManager(mouseManager)
        , mLeftStickX(0)
        , mLeftStickY(0)
        , mLeftStickDpadButton(kLeftStickDpadNone)
        , mGuiNavHeldButton(kLeftStickDpadNone)
        , mGuiNavHoldTime(0.f)
        , mGuiNavRepeatTime(0.f)
        , mGuiNavHeldFromAxis(false)
        , mIgnoreGuiNavUntilRelease(false)
        , mIgnoreGuiCursorUntilRelease(false)
        , mGyroAvailable(false)
        , mGamepadGuiCursorEnabled(true)
        , mGuiCursorEnabled(true)
        , mJoystickLastUsed(false)
        , mGamepadMousePressed(false)
        , mLeftTriggerPressed(false)
        , mRightTriggerPressed(false)
        , mLeftTriggerHoldTime(0.f)
        , mRightTriggerHoldTime(0.f)
        , mLeftTriggerRepeatTime(0.f)
        , mRightTriggerRepeatTime(0.f)
        , mPendingDialogueTriggerScrollTicks(0.f)
    {
        if (!controllerBindingsFile.empty())
        {
            const int result
                = SDL_GameControllerAddMappingsFromFile(Files::pathToUnicodeString(controllerBindingsFile).c_str());
            if (result < 0)
                Log(Debug::Error) << "Failed to add game controller mappings from file \"" << controllerBindingsFile
                                  << "\": " << SDL_GetError();
        }

        if (!userControllerBindingsFile.empty())
        {
            const int result
                = SDL_GameControllerAddMappingsFromFile(Files::pathToUnicodeString(userControllerBindingsFile).c_str());
            if (result < 0)
                Log(Debug::Error) << "Failed to add game controller mappings from user file \""
                                  << userControllerBindingsFile << "\": " << SDL_GetError();
        }

        // Open all presently connected sticks
        const int numSticks = SDL_NumJoysticks();
        if (numSticks < 0)
            Log(Debug::Error) << "Failed to get number of joysticks: " << SDL_GetError();

        for (int i = 0; i < numSticks; i++)
        {
            if (SDL_IsGameController(i))
            {
                SDL_ControllerDeviceEvent evt{};
                evt.which = i;
                static const int fakeDeviceID = 1;
                ControllerManager::controllerAdded(fakeDeviceID, evt);
                if (const char* name = SDL_GameControllerNameForIndex(i))
                    Log(Debug::Info) << "Detected game controller: " << name;
                else
                    Log(Debug::Warning) << "Detected game controller without a name: " << SDL_GetError();
            }
            else
            {
                if (const char* name = SDL_JoystickNameForIndex(i))
                    Log(Debug::Info) << "Detected unusable controller: " << name;
                else
                    Log(Debug::Warning) << "Detected unusable controller without a name: " << SDL_GetError();
            }
        }

        mBindingsManager->setJoystickDeadZone(Settings::input().mJoystickDeadZone);
    }

    void ControllerManager::setGuiCursorEnabled(bool enabled)
    {
        if (mGuiCursorEnabled == enabled)
            return;

        mGuiCursorEnabled = enabled;
        resetGuiNavState();
        mIgnoreGuiNavUntilRelease = false;
        mIgnoreGuiCursorUntilRelease = false;
        if (enabled)
        {
            SDL_GameController* cntrl = mBindingsManager->getControllerOrNull();
            if (cntrl)
            {
                const float xAxisRaw = SDL_GameControllerGetAxis(cntrl, SDL_CONTROLLER_AXIS_LEFTX) / kLeftStickAxisMax;
                const float yAxisRaw = SDL_GameControllerGetAxis(cntrl, SDL_CONTROLLER_AXIS_LEFTY) / kLeftStickAxisMax;
                float xAxis = xAxisRaw;
                float yAxis = yAxisRaw;
                applyRadialDeadzone(xAxis, yAxis, kLeftStickDpadDeadzone, xAxis, yAxis);
                if (xAxis != 0.f || yAxis != 0.f)
                    mIgnoreGuiNavUntilRelease = true;
                xAxis = xAxisRaw;
                yAxis = yAxisRaw;
                applyRadialDeadzone(xAxis, yAxis, kGuiJoystickDeadzone, xAxis, yAxis);
                if (xAxis != 0.f || yAxis != 0.f)
                    mIgnoreGuiCursorUntilRelease = true;
            }
        }
        else
            syncControllerAxisState();
    }

    void ControllerManager::resetGuiNavState()
    {
        mLeftStickX = 0;
        mLeftStickY = 0;
        mLeftStickDpadButton = kLeftStickDpadNone;
        mGuiNavHeldButton = kLeftStickDpadNone;
        mGuiNavHoldTime = 0.f;
        mGuiNavRepeatTime = 0.f;
        mGuiNavHeldFromAxis = false;
    }

    void ControllerManager::syncControllerAxisState()
    {
        if (mLastControllerDeviceId < 0)
            return;
        SDL_GameController* cntrl = mBindingsManager->getControllerOrNull();
        if (!cntrl)
            return;

        auto sendAxis = [&](SDL_GameControllerAxis axis) {
            SDL_ControllerAxisEvent event{};
            event.type = SDL_CONTROLLERAXISMOTION;
            event.which = mLastControllerDeviceId;
            event.axis = static_cast<Uint8>(axis);
            event.value = SDL_GameControllerGetAxis(cntrl, axis);
            mBindingsManager->controllerAxisMoved(mLastControllerDeviceId, event);
        };

        sendAxis(SDL_CONTROLLER_AXIS_LEFTX);
        sendAxis(SDL_CONTROLLER_AXIS_LEFTY);
        sendAxis(SDL_CONTROLLER_AXIS_RIGHTX);
        sendAxis(SDL_CONTROLLER_AXIS_RIGHTY);
        sendAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        sendAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    }

    void ControllerManager::update(float dt)
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        bool leftStickDpadActive = false;

        const bool inGuiMode = winMgr->isGuiMode();
        MWGui::WindowBase* activeWindow = inGuiMode ? winMgr->getActiveControllerWindow() : nullptr;
        if (inGuiMode != mLastGuiMode || activeWindow != mLastActiveControllerWindow)
        {
            mLastGuiMode = inGuiMode;
            mLastActiveControllerWindow = activeWindow;
            if (inGuiMode && activeWindow && activeWindow->isVisible())
            {
                SDL_GameController* cntrl = mBindingsManager->getControllerOrNull();
                if (cntrl)
                {
                    const float xAxisRaw
                        = SDL_GameControllerGetAxis(cntrl, SDL_CONTROLLER_AXIS_LEFTX) / kLeftStickAxisMax;
                    const float yAxisRaw
                        = SDL_GameControllerGetAxis(cntrl, SDL_CONTROLLER_AXIS_LEFTY) / kLeftStickAxisMax;
                    float xAxis = xAxisRaw;
                    float yAxis = yAxisRaw;
                    applyRadialDeadzone(xAxis, yAxis, kLeftStickDpadDeadzone, xAxis, yAxis);
                    if (xAxis != 0.f || yAxis != 0.f)
                    {
                        if (mGuiNavHeldButton != kLeftStickDpadNone)
                        {
                            const MyGUI::KeyCode releaseKey = dpadButtonToKey(mGuiNavHeldButton);
                            if (releaseKey != MyGUI::KeyCode::None)
                                winMgr->injectKeyRelease(releaseKey);
                        }
                        resetGuiNavState();
                        mIgnoreGuiNavUntilRelease = true;
                    }
                    xAxis = xAxisRaw;
                    yAxis = yAxisRaw;
                    applyRadialDeadzone(xAxis, yAxis, kGuiJoystickDeadzone, xAxis, yAxis);
                    if (xAxis != 0.f || yAxis != 0.f)
                        mIgnoreGuiCursorUntilRelease = true;
                }
            }
        }

        const bool mapWindowActive = inGuiMode && dynamic_cast<MWGui::MapWindow*>(activeWindow) != nullptr;
        if (mapWindowActive)
            mIgnoreGuiCursorUntilRelease = false;

        const bool settingsVisible = winMgr->isSettingsWindowVisible();
        const bool allowGuiMouseInThisContext = (mGamepadGuiCursorEnabled || settingsVisible);

        const bool allowJoystickDpadInMode
            = !isCharGenMouseAllowed(winMgr->getMode()) || winMgr->isVirtualKeyboardVisible();
        if (Settings::gui().mControllerMenus && Settings::gui().mControllerJoystickDpad && winMgr->isGuiMode()
            && !winMgr->isSettingsWindowVisible() && allowJoystickDpadInMode)
        {
            if (MWGui::WindowBase* topWin = winMgr->getActiveControllerWindow())
                leftStickDpadActive = topWin->isVisible() && dynamic_cast<MWGui::MapWindow*>(topWin) == nullptr;
        }
        if (Settings::gui().mControllerMenus && winMgr->isGuiMode() && !settingsVisible
            && isDialogueMode(winMgr->getMode()))
        {
            if (MWGui::WindowBase* topWin = winMgr->getActiveControllerWindow())
            {
                if (topWin->isVisible())
                {
                    if (mJoystickLastUsed && !topWin->isGamepadCursorAllowed())
                    {
                        winMgr->setCursorActive(false);
                        winMgr->setCursorVisible(false);
                    }
                }
            }
        }

        if (mIgnoreGuiCursorUntilRelease && winMgr->isGuiMode())
        {
            SDL_GameController* cntrl = mBindingsManager->getControllerOrNull();
            if (!cntrl)
                mIgnoreGuiCursorUntilRelease = false;
            else
            {
                float xAxis = SDL_GameControllerGetAxis(cntrl, SDL_CONTROLLER_AXIS_LEFTX) / kLeftStickAxisMax;
                float yAxis = SDL_GameControllerGetAxis(cntrl, SDL_CONTROLLER_AXIS_LEFTY) / kLeftStickAxisMax;
                applyRadialDeadzone(xAxis, yAxis, kGuiJoystickDeadzone, xAxis, yAxis);
                if (xAxis == 0.f && yAxis == 0.f)
                    mIgnoreGuiCursorUntilRelease = false;
            }
        }

        if (mGuiNavHeldFromAxis && Settings::gui().mControllerMenus && Settings::gui().mControllerJoystickDpad
            && winMgr->isGuiMode() && !winMgr->isSettingsWindowVisible() && allowJoystickDpadInMode)
        {
            SDL_GameController* cntrl = mBindingsManager->getControllerOrNull();
            if (cntrl)
            {
                float xAxis = SDL_GameControllerGetAxis(cntrl, SDL_CONTROLLER_AXIS_LEFTX) / kLeftStickAxisMax;
                float yAxis = SDL_GameControllerGetAxis(cntrl, SDL_CONTROLLER_AXIS_LEFTY) / kLeftStickAxisMax;
                applyRadialDeadzone(xAxis, yAxis, kLeftStickDpadReleaseDeadzone, xAxis, yAxis);
                if (xAxis == 0.f && yAxis == 0.f)
                {
                    const int lastButton = mGuiNavHeldButton;
                    mLeftStickDpadButton = kLeftStickDpadNone;
                    mGuiNavHeldButton = kLeftStickDpadNone;
                    mGuiNavHoldTime = 0.f;
                    mGuiNavRepeatTime = 0.f;
                    mGuiNavHeldFromAxis = false;
                    const MyGUI::KeyCode releaseKey = dpadButtonToKey(lastButton);
                    if (releaseKey != MyGUI::KeyCode::None)
                        winMgr->injectKeyRelease(releaseKey);
                }
            }
        }

        if (mGuiNavHeldButton != kLeftStickDpadNone)
        {
            const float repeatRate = Settings::gui().mControllerMenuTurboRepeat * 7.5f;
            const bool repeatEnabled = repeatRate > 0.f;
            if (!repeatEnabled)
            {
                mGuiNavHoldTime = 0.f;
                mGuiNavRepeatTime = 0.f;
            }
            else if (winMgr->isGuiMode() && !winMgr->isSettingsWindowVisible())
            {
                mGuiNavHoldTime += dt;
                if (mGuiNavHoldTime >= kGuiTurboRepeatStartDelay)
                {
                    const float interval = 1.f / repeatRate;
                    mGuiNavRepeatTime += dt;
                    while (mGuiNavRepeatTime >= interval)
                    {
                        mGuiNavRepeatTime -= interval;
                        SDL_ControllerButtonEvent buttonEvent{};
                        buttonEvent.type = SDL_CONTROLLERBUTTONDOWN;
                        buttonEvent.state = SDL_PRESSED;
                        buttonEvent.button = static_cast<Uint8>(mGuiNavHeldButton);
                        gamepadToGuiControl(buttonEvent);
                    }
                }
            }
        }

        if (Settings::gui().mControllerMenus && winMgr->isGuiMode() && !settingsVisible)
        {
            MWGui::WindowBase* topWin = winMgr->getActiveControllerWindow();
            const bool canRepeatScroll = usesTriggerScroll(topWin, winMgr);
            if (!canRepeatScroll)
                mPendingDialogueTriggerScrollTicks = 0.f;
            const float repeatRate = Settings::gui().mControllerMenuTurboRepeat * 7.5f;
            const bool repeatEnabled = repeatRate > 0.f;

            auto repeatTriggerScroll = [&](bool pressed, float& holdTime, float& repeatTime, Sint16 axisValue) {
                if (!pressed || !canRepeatScroll || !repeatEnabled)
                {
                    holdTime = 0.f;
                    repeatTime = 0.f;
                    return;
                }

                holdTime += dt;
                if (holdTime < kGuiTurboRepeatStartDelay)
                    return;

                const float interval = 1.f / repeatRate;
                repeatTime += dt;
                while (repeatTime >= interval)
                {
                    repeatTime -= interval;
                    mPendingDialogueTriggerScrollTicks
                        += axisValue < 0 ? -kDialogueTriggerScrollTicks : kDialogueTriggerScrollTicks;
                }
            };

            repeatTriggerScroll(mLeftTriggerPressed, mLeftTriggerHoldTime, mLeftTriggerRepeatTime, -32767);
            repeatTriggerScroll(mRightTriggerPressed, mRightTriggerHoldTime, mRightTriggerRepeatTime, 32767);

            if (!mLeftTriggerPressed && !mRightTriggerPressed)
                mPendingDialogueTriggerScrollTicks = 0.f;

            if (canRepeatScroll && mPendingDialogueTriggerScrollTicks != 0.f)
            {
                const int maxTicksThisFrame
                    = std::max(1, static_cast<int>(std::lround(kDialogueTriggerScrollSmoothRate * dt)));
                const int pendingTicks = static_cast<int>(std::abs(mPendingDialogueTriggerScrollTicks));
                const int ticksToSend = std::max(1, std::min(maxTicksThisFrame, pendingTicks));
                const bool scrollUp = mPendingDialogueTriggerScrollTicks < 0.f;

                SDL_ControllerAxisEvent scrollEvent{};
                scrollEvent.type = SDL_CONTROLLERAXISMOTION;
                scrollEvent.axis
                    = static_cast<Uint8>(scrollUp ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
                scrollEvent.value = 32767;
                for (int i = 0; i < ticksToSend; ++i)
                    topWin->onControllerThumbstickEvent(scrollEvent);

                mPendingDialogueTriggerScrollTicks += scrollUp ? ticksToSend : -ticksToSend;
                if (std::abs(mPendingDialogueTriggerScrollTicks) < 1.f)
                    mPendingDialogueTriggerScrollTicks = 0.f;
            }
        }
        else
        {
            mLeftTriggerHoldTime = 0.f;
            mRightTriggerHoldTime = 0.f;
            mLeftTriggerRepeatTime = 0.f;
            mRightTriggerRepeatTime = 0.f;
            mPendingDialogueTriggerScrollTicks = 0.f;
        }

        if (mGuiCursorEnabled && !(mJoystickLastUsed && !allowGuiMouseInThisContext) && !leftStickDpadActive)
        {
            float xAxis = mBindingsManager->getActionValue(A_MoveLeftRight) * 2.0f - 1.0f;
            float yAxis = mBindingsManager->getActionValue(A_MoveForwardBackward) * 2.0f - 1.0f;
            float zAxis = mBindingsManager->getActionValue(A_LookUpDown) * 2.0f - 1.0f;

            if (winMgr->isGuiMode())
            {
                if (mIgnoreGuiCursorUntilRelease)
                {
                    xAxis = 0.f;
                    yAxis = 0.f;
                }
                applyRadialDeadzone(xAxis, yAxis, kGuiJoystickDeadzone, xAxis, yAxis);
                const float joystickDeadZone = Settings::input().mJoystickDeadZone;
                const float scrollDeadzone = joystickDeadZone > 0.2f ? joystickDeadZone : 0.2f;
                if (std::abs(zAxis) < scrollDeadzone)
                    zAxis = 0.f;
            }

            xAxis *= (1.5f - mBindingsManager->getActionValue(A_Use));
            yAxis *= (1.5f - mBindingsManager->getActionValue(A_Use));

            float uiScale = winMgr->getScalingFactor();
            const float gamepadCursorSpeed = Settings::input().mGamepadCursorSpeed;
            const float xMove = xAxis * dt * 1500.0f / uiScale * gamepadCursorSpeed;
            const float yMove = yAxis * dt * 1500.0f / uiScale * gamepadCursorSpeed;

            float mouseWheelMove = -zAxis * dt * 1500.0f;
            if (xMove != 0 || yMove != 0 || mouseWheelMove != 0)
            {
                mMouseManager->injectMouseMove(xMove, yMove, mouseWheelMove);
                mMouseManager->warpMouse();
                winMgr->setCursorActive(true);
            }
        }

        if (!winMgr->isGuiMode()
            && MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_Running
            && MWBase::Environment::get().getInputManager()->getControlSwitch("playercontrols"))
        {
            float xAxis = mBindingsManager->getActionValue(A_MoveLeftRight);
            float yAxis = mBindingsManager->getActionValue(A_MoveForwardBackward);
            if (xAxis != 0.5 || yAxis != 0.5)
            {
                mJoystickLastUsed = true;
                MWBase::Environment::get().getInputManager()->resetIdleTime();
            }
        }
    }

    void ControllerManager::buttonPressed(int deviceID, const SDL_ControllerButtonEvent& arg)
    {
        if (!Settings::input().mEnableController || mBindingsManager->isDetectingBindingState())
            return;

        if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
        {
            MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
            if (winMgr->isGuiMode() && isInventoryMode(winMgr->getMode()))
            {
                bool& held
                    = (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? mLeftShoulderHeld : mRightShoulderHeld;
                if (held)
                    return;
                held = true;
            }
        }

        mLastControllerDeviceId = deviceID;
        MWBase::Environment::get().getLuaManager()->inputEvent(
            { MWBase::LuaManager::InputEvent::ControllerPressed, arg.button });

        mJoystickLastUsed = true;

        // While controls are disabled (e.g., startup videos), ignore normal GUI/controller handling
        // and only allow the synthetic Escape press used to skip videos.
        if (MWBase::Environment::get().getInputManager()->controlsDisabled())
        {
            auto kc = SDLUtil::sdlKeyToMyGUI(SDLK_ESCAPE);
            MyGUI::InputManager::getInstance().injectKeyPress(kc, 0);
            return;
        }

        if (isDpadButton(arg.button))
        {
            if (mGuiNavHeldButton == arg.button && !mGuiNavHeldFromAxis)
                return;
            mGuiNavHeldButton = arg.button;
            mGuiNavHeldFromAxis = false;
            mGuiNavHoldTime = 0.f;
            mGuiNavRepeatTime = 0.f;
        }
        if (MWBase::Environment::get().getWindowManager()->isGuiMode())
        {
            if (gamepadToGuiControl(arg))
                return;

            if (mGamepadGuiCursorEnabled)
            {
                // Temporary mouse binding until keyboard controls are available:
                if (arg.button == SDL_CONTROLLER_BUTTON_A) // We'll pretend that A is left click.
                {
                    constexpr int kSoftwareMouseFocusAssistSize = 32;
                    constexpr int kSoftwareMouseFocusAssistLeft = kSoftwareMouseFocusAssistSize / 2;
                    constexpr int kSoftwareMouseFocusAssistUp = kSoftwareMouseFocusAssistSize / 2;
                    constexpr int kSoftwareMouseFocusAssistRight
                        = kSoftwareMouseFocusAssistSize - 1 - kSoftwareMouseFocusAssistLeft;
                    constexpr int kSoftwareMouseFocusAssistDown
                        = kSoftwareMouseFocusAssistSize - 1 - kSoftwareMouseFocusAssistUp;
                    const bool useFocusAssist
                        = Settings::input().mEnableSoftwareMouse && Settings::gui().mControllerMenus;
                    bool mousePressSuccess = useFocusAssist
                        ? mMouseManager->injectMouseButtonPressWithFocusAssist(SDL_BUTTON_LEFT,
                            kSoftwareMouseFocusAssistLeft, kSoftwareMouseFocusAssistUp, kSoftwareMouseFocusAssistRight,
                            kSoftwareMouseFocusAssistDown)
                        : mMouseManager->injectMouseButtonPress(SDL_BUTTON_LEFT);
                    mGamepadMousePressed = true;
                    MWBase::Environment::get().getWindowManager()->setCursorActive(true);
                    if (MyGUI::InputManager::getInstance().getMouseFocusWidget())
                    {
                        MyGUI::Button* b
                            = MyGUI::InputManager::getInstance().getMouseFocusWidget()->castType<MyGUI::Button>(false);
                        if (b && b->getEnabled())
                            MWBase::Environment::get().getWindowManager()->playSound(
                                ESM::RefId::stringRefId("Menu Click"));
                    }

                    mBindingsManager->setPlayerControlsEnabled(!mousePressSuccess);
                }
            }
        }
        else
            mBindingsManager->setPlayerControlsEnabled(true);

        // esc, to leave initial movie screen
        auto kc = SDLUtil::sdlKeyToMyGUI(SDLK_ESCAPE);
        mBindingsManager->setPlayerControlsEnabled(!MyGUI::InputManager::getInstance().injectKeyPress(kc, 0));

        if (!MWBase::Environment::get().getInputManager()->controlsDisabled())
            mBindingsManager->controllerButtonPressed(deviceID, arg);
    }

    void ControllerManager::buttonReleased(int deviceID, const SDL_ControllerButtonEvent& arg)
    {
        if (mBindingsManager->isDetectingBindingState())
        {
            mBindingsManager->controllerButtonReleased(deviceID, arg);
            return;
        }

        mLastControllerDeviceId = deviceID;
        if (Settings::input().mEnableController)
        {
            MWBase::Environment::get().getLuaManager()->inputEvent(
                { MWBase::LuaManager::InputEvent::ControllerReleased, arg.button });
        }

        if (MWBase::Environment::get().getInputManager()->controlsDisabled())
        {
            auto kc = SDLUtil::sdlKeyToMyGUI(SDLK_ESCAPE);
            MyGUI::InputManager::getInstance().injectKeyRelease(kc);
            return;
        }

        if (!Settings::input().mEnableController)
            return;

        mJoystickLastUsed = true;
        if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
            mLeftShoulderHeld = false;
        else if (arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
            mRightShoulderHeld = false;
        if (MWBase::Environment::get().getWindowManager()->isGuiMode() && Settings::gui().mControllerMenus)
        {
            if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
            {
                MWGui::WindowBase* activeWindow
                    = MWBase::Environment::get().getWindowManager()->getActiveControllerWindow();
                if (activeWindow && activeWindow->isVisible()
                    && dynamic_cast<MWGui::RaceDialog*>(activeWindow) != nullptr)
                    activeWindow->onControllerButtonEvent(arg);
            }

            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
                MWBase::Environment::get().getWindowManager()->injectKeyRelease(MyGUI::KeyCode::ArrowUp);
            else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                MWBase::Environment::get().getWindowManager()->injectKeyRelease(MyGUI::KeyCode::ArrowDown);
            else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
                MWBase::Environment::get().getWindowManager()->injectKeyRelease(MyGUI::KeyCode::ArrowLeft);
            else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
                MWBase::Environment::get().getWindowManager()->injectKeyRelease(MyGUI::KeyCode::ArrowRight);
            if (arg.button == SDL_CONTROLLER_BUTTON_Y)
            {
                MWGui::WindowBase* activeWindow
                    = MWBase::Environment::get().getWindowManager()->getActiveControllerWindow();
                if (!dynamic_cast<MWGui::MapWindow*>(activeWindow))
                    MWBase::Environment::get().getWindowManager()->setControllerTooltipEnabled(false);
            }
        }
        if (isDpadButton(arg.button) && !mGuiNavHeldFromAxis && mGuiNavHeldButton == arg.button)
        {
            mGuiNavHeldButton = kLeftStickDpadNone;
            mGuiNavHoldTime = 0.f;
            mGuiNavRepeatTime = 0.f;
        }
        if (MWBase::Environment::get().getWindowManager()->isGuiMode())
        {
            if (mGamepadGuiCursorEnabled && (!Settings::gui().mControllerMenus || mGamepadMousePressed))
            {
                // Temporary mouse binding until keyboard controls are available:
                if (arg.button == SDL_CONTROLLER_BUTTON_A) // We'll pretend that A is left click.
                {
                    bool mousePressSuccess = mMouseManager->injectMouseButtonRelease(SDL_BUTTON_LEFT);
                    mGamepadMousePressed = false;
                    if (mBindingsManager->isDetectingBindingState()) // If the player just triggered binding, don't let
                                                                     // button release bind.
                        return;

                    mBindingsManager->setPlayerControlsEnabled(!mousePressSuccess);
                }
            }
        }
        else
            mBindingsManager->setPlayerControlsEnabled(true);

        // esc, to leave initial movie screen
        auto kc = SDLUtil::sdlKeyToMyGUI(SDLK_ESCAPE);
        mBindingsManager->setPlayerControlsEnabled(!MyGUI::InputManager::getInstance().injectKeyRelease(kc));

        mBindingsManager->controllerButtonReleased(deviceID, arg);
    }

    void ControllerManager::axisMoved(int deviceID, const SDL_ControllerAxisEvent& arg)
    {
        if (mBindingsManager->isDetectingBindingState())
        {
            mBindingsManager->controllerAxisMoved(deviceID, arg);
            return;
        }

        if (!Settings::input().mEnableController || MWBase::Environment::get().getInputManager()->controlsDisabled())
            return;

        mLastControllerDeviceId = deviceID;
        mJoystickLastUsed = true;
        if (MWBase::Environment::get().getWindowManager()->isGuiMode())
        {
            if (gamepadToGuiControl(arg))
                return;
        }
        else if (mBindingsManager->actionIsActive(A_TogglePOV)
            && (arg.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT || arg.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT))
        {
            // Preview Mode Gamepad Zooming; do not propagate to mBindingsManager
            return;
        }
        mBindingsManager->controllerAxisMoved(deviceID, arg);
    }

    void ControllerManager::controllerAdded(int deviceID, const SDL_ControllerDeviceEvent& arg)
    {
        mBindingsManager->controllerAdded(deviceID, arg);
        enableGyroSensor();
    }

    void ControllerManager::controllerRemoved(const SDL_ControllerDeviceEvent& arg)
    {
        mBindingsManager->controllerRemoved(arg);
    }

    bool ControllerManager::gamepadToGuiControl(const SDL_ControllerButtonEvent& arg)
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();

        if (Settings::gui().mControllerMenus)
        {
            // Block Start while the virtual keyboard is open.
            if (arg.button == SDL_CONTROLLER_BUTTON_START && winMgr->isVirtualKeyboardVisible())
                return true;

            MWGui::WindowBase* topWin = winMgr->getActiveControllerWindow();
            const bool mapWindowActive = dynamic_cast<MWGui::MapWindow*>(topWin) != nullptr;
            const bool isMapDpad = mapWindowActive && isDpadButton(arg.button);

            // Update cursor state.
            bool treatAsMouse = winMgr->getCursorVisible();
            if (!isMapDpad)
                winMgr->setCursorActive(false);

            if (arg.button == SDL_CONTROLLER_BUTTON_Y)
            {
                const bool enable = (arg.state == SDL_PRESSED);
                if (!mapWindowActive)
                    winMgr->setControllerTooltipEnabled(enable);
            }

            if (topWin && topWin->isVisible())
            {
                // When the inventory tooltip is visible, we don't actually want the A button to
                // act like a mouse button; it should act normally.
                if (treatAsMouse && arg.button == SDL_CONTROLLER_BUTTON_A && winMgr->getControllerTooltipVisible())
                    treatAsMouse = false;

                const bool settingsCursorOverride
                    = winMgr->isSettingsWindowVisible() && !MyGUI::InputManager::getInstance().isModalAny();
                mGamepadGuiCursorEnabled = settingsCursorOverride ? true : topWin->isGamepadCursorAllowed();

                // Fall through to mouse click
                if (mGamepadGuiCursorEnabled && treatAsMouse && arg.button == SDL_CONTROLLER_BUTTON_A)
                    return false;

                if (topWin->onControllerButtonEvent(arg))
                    return true;
            }
        }

        // Presumption of GUI mode will be removed in the future.
        // MyGUI KeyCodes *may* change.
        MyGUI::KeyCode key = MyGUI::KeyCode::None;
        switch (arg.button)
        {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                key = MyGUI::KeyCode::ArrowUp;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                key = MyGUI::KeyCode::ArrowRight;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                key = MyGUI::KeyCode::ArrowDown;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                key = MyGUI::KeyCode::ArrowLeft;
                break;
            case SDL_CONTROLLER_BUTTON_A:
                // If we are using the joystick as a GUI mouse, A must be handled via mouse.
                if (mGamepadGuiCursorEnabled)
                    return false;
                key = MyGUI::KeyCode::Space;
                break;
            case SDL_CONTROLLER_BUTTON_B:
                if (MyGUI::InputManager::getInstance().isModalAny())
                    winMgr->exitCurrentModal();
                else
                    winMgr->exitCurrentGuiMode();
                return true;
            case SDL_CONTROLLER_BUTTON_X:
                key = MyGUI::KeyCode::Semicolon;
                break;
            case SDL_CONTROLLER_BUTTON_Y:
                key = MyGUI::KeyCode::Apostrophe;
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                MyGUI::InputManager::getInstance().injectKeyPress(MyGUI::KeyCode::LeftShift);
                winMgr->injectKeyPress(MyGUI::KeyCode::Tab, 0, false);
                MyGUI::InputManager::getInstance().injectKeyRelease(MyGUI::KeyCode::LeftShift);
                return true;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                MWBase::Environment::get().getWindowManager()->injectKeyPress(MyGUI::KeyCode::Tab, 0, false);
                return true;
            case SDL_CONTROLLER_BUTTON_LEFTSTICK:
                mGamepadGuiCursorEnabled = !mGamepadGuiCursorEnabled;
                winMgr->setCursorActive(mGamepadGuiCursorEnabled);
                return true;
            default:
                return false;
        }

        // Some keys will work even when Text Input windows/modals are in focus.
        if (SDL_IsTextInputActive())
            return false;

        winMgr->injectKeyPress(key, 0, false);
        return true;
    }

    bool ControllerManager::gamepadToGuiControl(const SDL_ControllerAxisEvent& arg)
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();

        if (arg.axis == SDL_CONTROLLER_AXIS_LEFTX)
            mLeftStickX = arg.value;
        else if (arg.axis == SDL_CONTROLLER_AXIS_LEFTY)
            mLeftStickY = arg.value;

        if (mIgnoreGuiNavUntilRelease
            && (arg.axis == SDL_CONTROLLER_AXIS_LEFTX || arg.axis == SDL_CONTROLLER_AXIS_LEFTY))
        {
            float xAxis = mLeftStickX / kLeftStickAxisMax;
            float yAxis = mLeftStickY / kLeftStickAxisMax;
            applyRadialDeadzone(xAxis, yAxis, kLeftStickDpadReleaseDeadzone, xAxis, yAxis);
            if (xAxis == 0.f && yAxis == 0.f)
                mIgnoreGuiNavUntilRelease = false;
            return false;
        }

        if (winMgr->isVirtualKeyboardVisible()
            && (arg.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT || arg.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT))
        {
            MWGui::WindowBase* topWin = winMgr->getActiveControllerWindow();
            if (topWin && topWin->isVisible() && topWin->onControllerThumbstickEvent(arg))
                return true;
            return true;
        }
        if (MyGUI::InputManager::getInstance().isModalAny()
            && (arg.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT || arg.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT))
        {
            if (Settings::gui().mControllerMenus)
            {
                MWGui::WindowBase* topWin = winMgr->getActiveControllerWindow();
                if (topWin && topWin->isVisible())
                    (void)topWin->onControllerThumbstickEvent(arg);
            }

            mLeftTriggerPressed = false;
            mRightTriggerPressed = false;
            mLeftTriggerHoldTime = 0.f;
            mRightTriggerHoldTime = 0.f;
            mLeftTriggerRepeatTime = 0.f;
            mRightTriggerRepeatTime = 0.f;
            mPendingDialogueTriggerScrollTicks = 0.f;
            return true;
        }

        if (Settings::gui().mControllerMenus)
        {
            MWGui::WindowBase* topWin = winMgr->getActiveControllerWindow();
            const bool useTriggerScroll = usesTriggerScroll(topWin, winMgr);
            const bool useTriggerPaging = usesTriggerPaging(topWin);
            auto scrollDialogueByTrigger = [&](Sint16 axisValue) {
                SDL_ControllerAxisEvent scrollEvent = arg;
                scrollEvent.axis = static_cast<Uint8>(
                    axisValue < 0 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
                scrollEvent.value = 32767;
                for (int i = 0; i < kDialogueTriggerImmediateTicks; ++i)
                    topWin->onControllerThumbstickEvent(scrollEvent);
                const int queuedTicks = std::max(0, kDialogueTriggerScrollTicks - kDialogueTriggerImmediateTicks);
                mPendingDialogueTriggerScrollTicks += axisValue < 0 ? -queuedTicks : queuedTicks;
            };
            auto triggerPaging = [&](Uint8 axis) {
                if (!topWin)
                    return;
                SDL_ControllerAxisEvent pagingEvent = arg;
                pagingEvent.axis = axis;
                pagingEvent.value = 32767;
                topWin->onControllerThumbstickEvent(pagingEvent);
            };
            const float triggerPressThresholdNorm = static_cast<float>(kTriggerPressThreshold) / kLeftStickAxisMax;
            auto cancelTriggerState = [](bool& pressed, float& holdTime, float& repeatTime) {
                pressed = false;
                holdTime = 0.f;
                repeatTime = 0.f;
            };

            // Left and right triggers toggle through open GUI windows.
            if (arg.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
            {
                if (useTriggerScroll)
                {
                    if (!mRightTriggerPressed && arg.value >= kTriggerPressThreshold)
                    {
                        if (mLeftTriggerPressed)
                        {
                            cancelTriggerState(mLeftTriggerPressed, mLeftTriggerHoldTime, mLeftTriggerRepeatTime);
                            mPendingDialogueTriggerScrollTicks = 0.f;
                        }
                        mRightTriggerPressed = true;
                        mRightTriggerHoldTime = kGuiTurboRepeatStartDelay;
                        mRightTriggerRepeatTime = 0.f;
                        scrollDialogueByTrigger(32767);
                    }
                }
                else if (useTriggerPaging)
                {
                    if (!mRightTriggerPressed && arg.value >= kTriggerPressThreshold)
                    {
                        if (mLeftTriggerPressed)
                            cancelTriggerState(mLeftTriggerPressed, mLeftTriggerHoldTime, mLeftTriggerRepeatTime);
                        mRightTriggerPressed = true;
                        mRightTriggerHoldTime = 0.f;
                        mRightTriggerRepeatTime = 0.f;
                        triggerPaging(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
                    }
                }
                else if (!mRightTriggerPressed && arg.value >= kTriggerPressThreshold)
                {
                    mRightTriggerPressed = true;
                    winMgr->cycleActiveControllerWindow(true);
                }

                if (mRightTriggerPressed && arg.value <= kTriggerReleaseThreshold)
                {
                    cancelTriggerState(mRightTriggerPressed, mRightTriggerHoldTime, mRightTriggerRepeatTime);
                    if (useTriggerScroll)
                    {
                        const float leftAxis = getAxisValue(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
                        if (!mLeftTriggerPressed && leftAxis >= triggerPressThresholdNorm)
                        {
                            mLeftTriggerPressed = true;
                            mLeftTriggerHoldTime = kGuiTurboRepeatStartDelay;
                            mLeftTriggerRepeatTime = 0.f;
                            scrollDialogueByTrigger(-32767);
                        }
                    }
                    else if (useTriggerPaging)
                    {
                        const float leftAxis = getAxisValue(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
                        if (!mLeftTriggerPressed && leftAxis >= triggerPressThresholdNorm)
                        {
                            mLeftTriggerPressed = true;
                            mLeftTriggerHoldTime = 0.f;
                            mLeftTriggerRepeatTime = 0.f;
                            triggerPaging(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
                        }
                    }
                }
                return true;
            }
            else if (arg.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
            {
                if (useTriggerScroll)
                {
                    if (!mLeftTriggerPressed && arg.value >= kTriggerPressThreshold)
                    {
                        if (mRightTriggerPressed)
                        {
                            cancelTriggerState(mRightTriggerPressed, mRightTriggerHoldTime, mRightTriggerRepeatTime);
                            mPendingDialogueTriggerScrollTicks = 0.f;
                        }
                        mLeftTriggerPressed = true;
                        mLeftTriggerHoldTime = kGuiTurboRepeatStartDelay;
                        mLeftTriggerRepeatTime = 0.f;
                        scrollDialogueByTrigger(-32767);
                    }
                }
                else if (useTriggerPaging)
                {
                    if (!mLeftTriggerPressed && arg.value >= kTriggerPressThreshold)
                    {
                        if (mRightTriggerPressed)
                            cancelTriggerState(mRightTriggerPressed, mRightTriggerHoldTime, mRightTriggerRepeatTime);
                        mLeftTriggerPressed = true;
                        mLeftTriggerHoldTime = 0.f;
                        mLeftTriggerRepeatTime = 0.f;
                        triggerPaging(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
                    }
                }
                else if (!mLeftTriggerPressed && arg.value >= kTriggerPressThreshold)
                {
                    mLeftTriggerPressed = true;
                    winMgr->cycleActiveControllerWindow(false);
                }

                if (mLeftTriggerPressed && arg.value <= kTriggerReleaseThreshold)
                {
                    cancelTriggerState(mLeftTriggerPressed, mLeftTriggerHoldTime, mLeftTriggerRepeatTime);
                    if (useTriggerScroll)
                    {
                        const float rightAxis = getAxisValue(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
                        if (!mRightTriggerPressed && rightAxis >= triggerPressThresholdNorm)
                        {
                            mRightTriggerPressed = true;
                            mRightTriggerHoldTime = kGuiTurboRepeatStartDelay;
                            mRightTriggerRepeatTime = 0.f;
                            scrollDialogueByTrigger(32767);
                        }
                    }
                    else if (useTriggerPaging)
                    {
                        const float rightAxis = getAxisValue(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
                        if (!mRightTriggerPressed && rightAxis >= triggerPressThresholdNorm)
                        {
                            mRightTriggerPressed = true;
                            mRightTriggerHoldTime = 0.f;
                            mRightTriggerRepeatTime = 0.f;
                            triggerPaging(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
                        }
                    }
                }
                return true;
            }

            if (topWin && topWin->isVisible())
            {
                const bool settingsCursorOverride
                    = winMgr->isSettingsWindowVisible() && !MyGUI::InputManager::getInstance().isModalAny();
                mGamepadGuiCursorEnabled = settingsCursorOverride ? true : topWin->isGamepadCursorAllowed();

                if (!mGamepadGuiCursorEnabled)
                    winMgr->setCursorActive(false);

                const bool mapWindowActive = dynamic_cast<MWGui::MapWindow*>(topWin) != nullptr;

                const bool allowJoystickDpad
                    = !isCharGenMouseAllowed(winMgr->getMode()) || winMgr->isVirtualKeyboardVisible();
                if (Settings::gui().mControllerJoystickDpad && !winMgr->isSettingsWindowVisible() && allowJoystickDpad
                    && !mapWindowActive
                    && (arg.axis == SDL_CONTROLLER_AXIS_LEFTX || arg.axis == SDL_CONTROLLER_AXIS_LEFTY))
                {
                    const float deadzone = (mLeftStickDpadButton == kLeftStickDpadNone) ? kLeftStickDpadDeadzone
                                                                                        : kLeftStickDpadReleaseDeadzone;
                    int nextButton = pickLeftStickDpadButton(mLeftStickX, mLeftStickY, deadzone);
                    if (mLeftStickDpadButton != kLeftStickDpadNone && nextButton != kLeftStickDpadNone
                        && nextButton != mLeftStickDpadButton)
                    {
                        nextButton = mLeftStickDpadButton;
                    }
                    if (nextButton != mLeftStickDpadButton)
                    {
                        const int prevButton = mLeftStickDpadButton;
                        mLeftStickDpadButton = nextButton;
                        if (prevButton != kLeftStickDpadNone)
                        {
                            const MyGUI::KeyCode releaseKey = dpadButtonToKey(prevButton);
                            if (releaseKey != MyGUI::KeyCode::None)
                                winMgr->injectKeyRelease(releaseKey);
                        }
                        if (nextButton != kLeftStickDpadNone)
                        {
                            mGamepadGuiCursorEnabled = false;
                            winMgr->setCursorActive(false);
                            winMgr->setCursorVisible(false);
                            SDL_ControllerButtonEvent buttonEvent{};
                            buttonEvent.type = SDL_CONTROLLERBUTTONDOWN;
                            buttonEvent.state = SDL_PRESSED;
                            buttonEvent.which = arg.which;
                            buttonEvent.button = static_cast<Uint8>(nextButton);
                            gamepadToGuiControl(buttonEvent);
                        }
                        if (nextButton != kLeftStickDpadNone)
                        {
                            mGuiNavHeldButton = nextButton;
                            mGuiNavHeldFromAxis = true;
                            mGuiNavHoldTime = 0.f;
                            mGuiNavRepeatTime = 0.f;
                        }
                        else if (mGuiNavHeldFromAxis)
                        {
                            mGuiNavHeldButton = kLeftStickDpadNone;
                            mGuiNavHoldTime = 0.f;
                            mGuiNavRepeatTime = 0.f;
                            mGuiNavHeldFromAxis = false;
                        }
                    }
                    else if (nextButton != kLeftStickDpadNone && mGuiNavHeldFromAxis)
                    {
                        mGuiNavHeldButton = nextButton;
                    }
                    return true;
                }
                else if (arg.axis == SDL_CONTROLLER_AXIS_LEFTX || arg.axis == SDL_CONTROLLER_AXIS_LEFTY)
                {
                    mLeftStickDpadButton = kLeftStickDpadNone;
                    if (mGuiNavHeldFromAxis)
                    {
                        mGuiNavHeldButton = kLeftStickDpadNone;
                        mGuiNavHoldTime = 0.f;
                        mGuiNavRepeatTime = 0.f;
                        mGuiNavHeldFromAxis = false;
                    }
                }

                // Deadzone check (skip for map GUI so fine movement is possible)
                if (!mapWindowActive && std::abs(arg.value) < 2000)
                    return !mGamepadGuiCursorEnabled;

                if (mGamepadGuiCursorEnabled
                    && (arg.axis == SDL_CONTROLLER_AXIS_LEFTX || arg.axis == SDL_CONTROLLER_AXIS_LEFTY))
                {
                    // Treat the left stick like a cursor, which is the default behavior.
                    winMgr->setControllerTooltipVisible(false);
                    winMgr->setCursorVisible(true);
                    return false;
                }

                // Some windows have a specific widget to scroll with the right stick. Move the mouse there.
                if (arg.axis == SDL_CONTROLLER_AXIS_RIGHTY && topWin->getControllerScrollWidget() != nullptr)
                {
                    mMouseManager->warpMouseToWidget(topWin->getControllerScrollWidget());
                    winMgr->setCursorVisible(false);
                }

                if (topWin->onControllerThumbstickEvent(arg))
                {
                    // Window handled the event.
                    return true;
                }
                else if (arg.axis == SDL_CONTROLLER_AXIS_RIGHTX || arg.axis == SDL_CONTROLLER_AXIS_RIGHTY)
                {
                    // Only right-stick scroll if mouse is visible or there's a widget to scroll.
                    if (mapWindowActive && Settings::map().mAllowZooming && arg.axis == SDL_CONTROLLER_AXIS_RIGHTY)
                        return false;
                    return !winMgr->getCursorVisible() && topWin->getControllerScrollWidget() == nullptr;
                }
            }
        }

        switch (arg.axis)
        {
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                if (arg.value == 32767) // Treat like a button.
                    winMgr->injectKeyPress(MyGUI::KeyCode::Minus, 0, false);
                break;
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                if (arg.value == 32767) // Treat like a button.
                    winMgr->injectKeyPress(MyGUI::KeyCode::Equals, 0, false);
                break;
            case SDL_CONTROLLER_AXIS_LEFTX:
            case SDL_CONTROLLER_AXIS_LEFTY:
            case SDL_CONTROLLER_AXIS_RIGHTX:
            case SDL_CONTROLLER_AXIS_RIGHTY:
                // If we are using the joystick as a GUI mouse, process mouse movement elsewhere.
                if (mGamepadGuiCursorEnabled)
                    return false;
                break;
            default:
                return false;
        }

        return true;
    }

    float ControllerManager::getAxisValue(SDL_GameControllerAxis axis) const
    {
        SDL_GameController* cntrl = mBindingsManager->getControllerOrNull();
        constexpr float axisMaxAbsoluteValue = 32768;
        if (cntrl != nullptr)
            return SDL_GameControllerGetAxis(cntrl, axis) / axisMaxAbsoluteValue;
        return 0;
    }

    bool ControllerManager::isButtonPressed(SDL_GameControllerButton button) const
    {
        SDL_GameController* cntrl = mBindingsManager->getControllerOrNull();
        if (cntrl)
            return SDL_GameControllerGetButton(cntrl, button) > 0;
        else
            return false;
    }

    void ControllerManager::enableGyroSensor()
    {
        mGyroAvailable = false;
        SDL_GameController* cntrl = mBindingsManager->getControllerOrNull();
        if (!cntrl)
            return;
        if (!SDL_GameControllerHasSensor(cntrl, SDL_SENSOR_GYRO))
            return;
        if (const int result = SDL_GameControllerSetSensorEnabled(cntrl, SDL_SENSOR_GYRO, SDL_TRUE); result < 0)
        {
            Log(Debug::Error) << "Failed to enable game controller sensor: " << SDL_GetError();
            return;
        }
        mGyroAvailable = true;
    }

    bool ControllerManager::isGyroAvailable() const
    {
        return mGyroAvailable;
    }

    std::array<float, 3> ControllerManager::getGyroValues() const
    {
        float gyro[3] = { 0.f };
        SDL_GameController* cntrl = mBindingsManager->getControllerOrNull();
        if (cntrl && mGyroAvailable)
        {
            const int result = SDL_GameControllerGetSensorData(cntrl, SDL_SENSOR_GYRO, gyro, 3);
            if (result < 0)
                Log(Debug::Error) << "Failed to get game controller sensor data: " << SDL_GetError();
        }
        return std::array<float, 3>({ gyro[0], gyro[1], gyro[2] });
    }

    int ControllerManager::getControllerType()
    {
        SDL_GameController* cntrl = mBindingsManager->getControllerOrNull();
        if (cntrl)
            return SDL_GameControllerGetType(cntrl);
        return 0;
    }

    std::string ControllerManager::getControllerButtonIcon(int button)
    {
        const ControllerIconStyle style = getControllerIconStyle();
        const bool useWhiteBlackButtonIcons = Settings::input().mControllerWhiteBlackButtonIcons;
        bool isXbox = false;
        bool isPsx = false;
        bool isSwitch = false;
        bool isGamecube = false;
        if (style == ControllerIconStyle::Auto)
        {
            const int controllerType = ControllerManager::getControllerType();
            isXbox = controllerType == SDL_CONTROLLER_TYPE_XBOX360 || controllerType == SDL_CONTROLLER_TYPE_XBOXONE;
            isPsx = controllerType == SDL_CONTROLLER_TYPE_PS3 || controllerType == SDL_CONTROLLER_TYPE_PS4
                || controllerType == SDL_CONTROLLER_TYPE_PS5;
            isSwitch = controllerType == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO;
        }
        else
        {
            isXbox = style == ControllerIconStyle::Xbox;
            isPsx = style == ControllerIconStyle::Playstation;
            isSwitch = style == ControllerIconStyle::Switch;
            isGamecube = style == ControllerIconStyle::Gamecube;
        }

        switch (button)
        {
            case SDL_CONTROLLER_BUTTON_A:
                if (isGamecube)
                    return "textures/omw_gamecube_button_a.dds";
                if (isSwitch)
                    return "textures/omw_switch_button_a.dds";
                if (isPsx)
                    return "textures/omw_psx_button_x.dds";
                return "textures/omw_steam_button_a.dds";
            case SDL_CONTROLLER_BUTTON_B:
                if (isGamecube)
                    return "textures/omw_gamecube_button_b.dds";
                if (isSwitch)
                    return "textures/omw_switch_button_b.dds";
                if (isPsx)
                    return "textures/omw_psx_button_circle.dds";
                return "textures/omw_steam_button_b.dds";
            case SDL_CONTROLLER_BUTTON_BACK:
                return "textures/omw_steam_button_view.dds";
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                if (isPsx)
                    return "textures/omw_psx_button_dpad.dds";
                return "textures/omw_steam_button_dpad.dds";
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                if (useWhiteBlackButtonIcons)
                    return "textures/omw_xbox_button_lb_WHITE.dds";
                if (isXbox)
                    return "textures/omw_xbox_button_lb.dds";
                else if (isSwitch)
                    return "textures/omw_switch_button_l.dds";
                return "textures/omw_steam_button_l1.dds";
            case SDL_CONTROLLER_BUTTON_LEFTSTICK:
                if (isGamecube)
                    return "textures/omw_gamecube_button_lstick.dds";
                return "textures/omw_steam_button_l3.dds";
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                if (useWhiteBlackButtonIcons)
                    return "textures/omw_xbox_button_rb_BLACK.dds";
                if (isGamecube)
                    return "textures/omw_gamecube_button_Z.dds";
                if (isXbox)
                    return "textures/omw_xbox_button_rb.dds";
                else if (isSwitch)
                    return "textures/omw_switch_button_r.dds";
                return "textures/omw_steam_button_r1.dds";
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
                return "textures/omw_steam_button_r3.dds";
            case SDL_CONTROLLER_BUTTON_START:
                return "textures/omw_steam_button_menu.dds";
            case SDL_CONTROLLER_BUTTON_X:
                if (isGamecube)
                    return "textures/omw_gamecube_button_x.dds";
                if (isSwitch)
                    return "textures/omw_switch_button_x.dds";
                if (isPsx)
                    return "textures/omw_psx_button_square.dds";
                return "textures/omw_steam_button_x.dds";
            case SDL_CONTROLLER_BUTTON_Y:
                if (isGamecube)
                    return "textures/omw_gamecube_button_y.dds";
                if (isSwitch)
                    return "textures/omw_switch_button_y.dds";
                if (isPsx)
                    return "textures/omw_psx_button_triangle.dds";
                return "textures/omw_steam_button_y.dds";
            case SDL_CONTROLLER_BUTTON_GUIDE:
            case SDL_CONTROLLER_BUTTON_MISC1:
            case SDL_CONTROLLER_BUTTON_PADDLE1:
            case SDL_CONTROLLER_BUTTON_PADDLE2:
            case SDL_CONTROLLER_BUTTON_PADDLE3:
            case SDL_CONTROLLER_BUTTON_PADDLE4:
            case SDL_CONTROLLER_BUTTON_TOUCHPAD:
            default:
                return {};
        }
    }

    std::string ControllerManager::getControllerAxisIcon(int axis)
    {
        const ControllerIconStyle style = getControllerIconStyle();
        bool isXbox = false;
        bool isSwitch = false;
        bool isGamecube = false;
        if (style == ControllerIconStyle::Auto)
        {
            const int controllerType = ControllerManager::getControllerType();
            isXbox = controllerType == SDL_CONTROLLER_TYPE_XBOX360 || controllerType == SDL_CONTROLLER_TYPE_XBOXONE;
            isSwitch = controllerType == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO;
        }
        else
        {
            isXbox = style == ControllerIconStyle::Xbox;
            isSwitch = style == ControllerIconStyle::Switch;
            isGamecube = style == ControllerIconStyle::Gamecube;
        }

        switch (axis)
        {
            case SDL_CONTROLLER_AXIS_LEFTX:
            case SDL_CONTROLLER_AXIS_LEFTY:
                if (isGamecube)
                    return "textures/omw_gamecube_button_lstick.dds";
                return "textures/omw_steam_button_lstick.dds";
            case SDL_CONTROLLER_AXIS_RIGHTX:
            case SDL_CONTROLLER_AXIS_RIGHTY:
                return "textures/omw_steam_button_rstick.dds";
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                if (isGamecube)
                    return "textures/omw_gamecube_button_lt.dds";
                if (isXbox)
                    return "textures/omw_xbox_button_lt.dds";
                else if (isSwitch)
                    return "textures/omw_switch_button_zl.dds";
                return "textures/omw_steam_button_l2.dds";
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                if (isGamecube)
                    return "textures/omw_gamecube_button_rt.dds";
                if (isXbox)
                    return "textures/omw_xbox_button_rt.dds";
                else if (isSwitch)
                    return "textures/omw_switch_button_zr.dds";
                return "textures/omw_steam_button_r2.dds";
            default:
                return {};
        }
    }

    void ControllerManager::touchpadMoved(int deviceId, const SDLUtil::TouchEvent& arg)
    {
        MWBase::Environment::get().getLuaManager()->inputEvent({ MWBase::LuaManager::InputEvent::TouchMoved, arg });
    }

    void ControllerManager::touchpadPressed(int deviceId, const SDLUtil::TouchEvent& arg)
    {
        MWBase::Environment::get().getLuaManager()->inputEvent({ MWBase::LuaManager::InputEvent::TouchPressed, arg });
    }

    void ControllerManager::touchpadReleased(int deviceId, const SDLUtil::TouchEvent& arg)
    {
        MWBase::Environment::get().getLuaManager()->inputEvent({ MWBase::LuaManager::InputEvent::TouchReleased, arg });
    }
}
