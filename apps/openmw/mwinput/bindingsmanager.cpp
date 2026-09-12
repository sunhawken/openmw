#include "bindingsmanager.hpp"

#include <algorithm>
#include <filesystem>

#include <MyGUI_EditBox.h>

#include <oics/ICSChannelListener.h>
#include <oics/ICSInputControlSystem.h>

#include <components/debug/debuglog.hpp>
#include <components/files/conversion.hpp>
#include <components/sdlutil/sdlmappings.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "actions.hpp"

namespace MWInput
{
    static const int sFakeDeviceId = 1; // As we only support one controller at a time, use a fake deviceID so we don't
                                        // lose bindings when switching controllers

    namespace
    {
        std::vector<ICS::Control*> findControlsBoundToKey(ICS::InputControlSystem* inputBinder, SDL_Scancode key)
        {
            std::vector<ICS::Control*> result;
            const int controlCount = inputBinder->getControlCount();
            result.reserve(static_cast<std::size_t>(controlCount));
            for (int i = 0; i < controlCount; ++i)
            {
                ICS::Control* control = inputBinder->getControl(i);
                if (inputBinder->getKeyBinding(control, ICS::Control::INCREASE) == key
                    || inputBinder->getKeyBinding(control, ICS::Control::DECREASE) == key)
                    result.push_back(control);
            }
            return result;
        }

        std::vector<ICS::Control*> findControlsBoundToMouseButton(
            ICS::InputControlSystem* inputBinder, unsigned int button)
        {
            std::vector<ICS::Control*> result;
            const int controlCount = inputBinder->getControlCount();
            result.reserve(static_cast<std::size_t>(controlCount));
            for (int i = 0; i < controlCount; ++i)
            {
                ICS::Control* control = inputBinder->getControl(i);
                if (inputBinder->getMouseButtonBinding(control, ICS::Control::INCREASE) == button
                    || inputBinder->getMouseButtonBinding(control, ICS::Control::DECREASE) == button)
                    result.push_back(control);
            }
            return result;
        }

        std::vector<ICS::Control*> findControlsBoundToMouseWheel(
            ICS::InputControlSystem* inputBinder, ICS::InputControlSystem::MouseWheelClick click)
        {
            std::vector<ICS::Control*> result;
            const int controlCount = inputBinder->getControlCount();
            result.reserve(static_cast<std::size_t>(controlCount));
            for (int i = 0; i < controlCount; ++i)
            {
                ICS::Control* control = inputBinder->getControl(i);
                if (inputBinder->getMouseWheelBinding(control, ICS::Control::INCREASE) == click
                    || inputBinder->getMouseWheelBinding(control, ICS::Control::DECREASE) == click)
                    result.push_back(control);
            }
            return result;
        }

        std::vector<ICS::Control*> findControlsBoundToJoystickButton(
            ICS::InputControlSystem* inputBinder, int deviceID, unsigned int button)
        {
            std::vector<ICS::Control*> result;
            const int controlCount = inputBinder->getControlCount();
            result.reserve(static_cast<std::size_t>(controlCount));
            for (int i = 0; i < controlCount; ++i)
            {
                ICS::Control* control = inputBinder->getControl(i);
                if (inputBinder->getJoystickButtonBinding(control, deviceID, ICS::Control::INCREASE) == button
                    || inputBinder->getJoystickButtonBinding(control, deviceID, ICS::Control::DECREASE) == button)
                    result.push_back(control);
            }
            return result;
        }

        std::vector<ICS::Control*> findControlsBoundToJoystickAxis(
            ICS::InputControlSystem* inputBinder, int deviceID, int axis)
        {
            std::vector<ICS::Control*> result;
            const int controlCount = inputBinder->getControlCount();
            result.reserve(static_cast<std::size_t>(controlCount));
            for (int i = 0; i < controlCount; ++i)
            {
                ICS::Control* control = inputBinder->getControl(i);
                if (inputBinder->getJoystickAxisBinding(control, deviceID, ICS::Control::INCREASE) == axis
                    || inputBinder->getJoystickAxisBinding(control, deviceID, ICS::Control::DECREASE) == axis)
                    result.push_back(control);
            }
            return result;
        }

        void resetControlsToInitialValue(const std::vector<ICS::Control*>& controls, const ICS::Control* exclude)
        {
            for (ICS::Control* control : controls)
            {
                if (control == nullptr || control == exclude)
                    continue;
                control->setChangingDirection(ICS::Control::STOP);
                control->setValue(control->getInitialValue());
            }
        }
    }

    void clearAllKeyBindings(ICS::InputControlSystem* inputBinder, ICS::Control* control)
    {
        // right now we don't really need multiple bindings for the same action, so remove all others first
        if (inputBinder->getKeyBinding(control, ICS::Control::INCREASE) != SDL_SCANCODE_UNKNOWN)
            inputBinder->removeKeyBinding(inputBinder->getKeyBinding(control, ICS::Control::INCREASE));
        if (inputBinder->getMouseButtonBinding(control, ICS::Control::INCREASE) != ICS_MAX_DEVICE_BUTTONS)
            inputBinder->removeMouseButtonBinding(inputBinder->getMouseButtonBinding(control, ICS::Control::INCREASE));
        if (inputBinder->getMouseWheelBinding(control, ICS::Control::INCREASE)
            != ICS::InputControlSystem::MouseWheelClick::UNASSIGNED)
            inputBinder->removeMouseWheelBinding(inputBinder->getMouseWheelBinding(control, ICS::Control::INCREASE));
    }

    void clearAllControllerBindings(ICS::InputControlSystem* inputBinder, ICS::Control* control)
    {
        // right now we don't really need multiple bindings for the same action, so remove all others first
        if (inputBinder->getJoystickAxisBinding(control, sFakeDeviceId, ICS::Control::INCREASE) != SDL_SCANCODE_UNKNOWN)
            inputBinder->removeJoystickAxisBinding(
                sFakeDeviceId, inputBinder->getJoystickAxisBinding(control, sFakeDeviceId, ICS::Control::INCREASE));
        if (inputBinder->getJoystickButtonBinding(control, sFakeDeviceId, ICS::Control::INCREASE)
            != ICS_MAX_DEVICE_BUTTONS)
            inputBinder->removeJoystickButtonBinding(
                sFakeDeviceId, inputBinder->getJoystickButtonBinding(control, sFakeDeviceId, ICS::Control::INCREASE));
    }

    class InputControlSystem : public ICS::InputControlSystem
    {
    public:
        InputControlSystem(const std::filesystem::path& bindingsFile)
            : ICS::InputControlSystem(Files::pathToUnicodeString(bindingsFile), true, nullptr, nullptr, A_Last)
        {
        }
    };

    class BindingsListener : public ICS::ChannelListener, public ICS::DetectingBindingListener
    {
    public:
        BindingsListener(ICS::InputControlSystem* inputBinder, BindingsManager* bindingsManager)
            : mInputBinder(inputBinder)
            , mBindingsManager(bindingsManager)
            , mDetectingKeyboard(false)
        {
        }

        virtual ~BindingsListener() = default;

        void channelChanged(ICS::Channel* channel, float currentValue, float previousValue) override
        {
            int action = channel->getNumber();
            mBindingsManager->actionValueChanged(action, currentValue, previousValue);
        }

        void keyBindingDetected(ICS::InputControlSystem* ics, ICS::Control* control, SDL_Scancode key,
            ICS::Control::ControlChangingDirection direction) override
        {
            // Disallow binding escape key
            if (key == SDL_SCANCODE_ESCAPE)
            {
                // Stop binding if esc pressed
                mInputBinder->cancelDetectingBindingState();
                MWBase::Environment::get().getWindowManager()->notifyInputActionBound();
                return;
            }

            // Disallow binding reserved keys
            if (key == SDL_SCANCODE_F3 || key == SDL_SCANCODE_F4 || key == SDL_SCANCODE_F10)
                return;

#ifndef __APPLE__
            // Disallow binding Windows/Meta keys
            if (key == SDL_SCANCODE_LGUI || key == SDL_SCANCODE_RGUI)
                return;
#endif

            if (!mDetectingKeyboard)
                return;

            const auto controlsPreviouslyBoundToKey = findControlsBoundToKey(mInputBinder, key);
            clearAllKeyBindings(mInputBinder, control);
            control->setInitialValue(0.0f);
            ICS::DetectingBindingListener::keyBindingDetected(ics, control, key, direction);
            resetControlsToInitialValue(controlsPreviouslyBoundToKey, control);
            MWBase::Environment::get().getWindowManager()->notifyInputActionBound();
        }

        void mouseAxisBindingDetected(ICS::InputControlSystem* /*ics*/, ICS::Control* /*control*/,
            ICS::InputControlSystem::NamedAxis /*axis*/, ICS::Control::ControlChangingDirection /*direction*/) override
        {
            // we don't want mouse movement bindings
            return;
        }

        void mouseButtonBindingDetected(ICS::InputControlSystem* ics, ICS::Control* control, unsigned int button,
            ICS::Control::ControlChangingDirection direction) override
        {
            if (!mDetectingKeyboard)
                return;
            const auto controlsPreviouslyBoundToButton = findControlsBoundToMouseButton(mInputBinder, button);
            clearAllKeyBindings(mInputBinder, control);
            control->setInitialValue(0.0f);
            ICS::DetectingBindingListener::mouseButtonBindingDetected(ics, control, button, direction);
            resetControlsToInitialValue(controlsPreviouslyBoundToButton, control);
            MWBase::Environment::get().getWindowManager()->notifyInputActionBound();
        }

        void mouseWheelBindingDetected(ICS::InputControlSystem* ics, ICS::Control* control,
            ICS::InputControlSystem::MouseWheelClick click, ICS::Control::ControlChangingDirection direction) override
        {
            if (!mDetectingKeyboard)
                return;
            const auto controlsPreviouslyBoundToWheel = findControlsBoundToMouseWheel(mInputBinder, click);
            clearAllKeyBindings(mInputBinder, control);
            control->setInitialValue(0.0f);
            ICS::DetectingBindingListener::mouseWheelBindingDetected(ics, control, click, direction);
            resetControlsToInitialValue(controlsPreviouslyBoundToWheel, control);
            MWBase::Environment::get().getWindowManager()->notifyInputActionBound();
        }

        void joystickAxisBindingDetected(ICS::InputControlSystem* ics, int deviceID, ICS::Control* control, int axis,
            ICS::Control::ControlChangingDirection direction) override
        {
            // only allow binding to the trigers
            if (axis != SDL_CONTROLLER_AXIS_TRIGGERLEFT && axis != SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
                return;
            if (mDetectingKeyboard)
                return;

            const auto controlsPreviouslyBoundToAxis = findControlsBoundToJoystickAxis(mInputBinder, deviceID, axis);
            clearAllControllerBindings(mInputBinder, control);
            control->setValue(0.5f); // axis bindings must start at 0.5
            control->setInitialValue(0.5f);
            ICS::DetectingBindingListener::joystickAxisBindingDetected(ics, deviceID, control, axis, direction);
            resetControlsToInitialValue(controlsPreviouslyBoundToAxis, control);
            MWBase::Environment::get().getWindowManager()->notifyInputActionBound();
        }

        void joystickButtonBindingDetected(ICS::InputControlSystem* ics, int deviceID, ICS::Control* control,
            unsigned int button, ICS::Control::ControlChangingDirection direction) override
        {
            if (mDetectingKeyboard)
                return;
            const auto controlsPreviouslyBoundToButton
                = findControlsBoundToJoystickButton(mInputBinder, deviceID, button);
            clearAllControllerBindings(mInputBinder, control);
            control->setInitialValue(0.0f);
            ICS::DetectingBindingListener::joystickButtonBindingDetected(ics, deviceID, control, button, direction);
            resetControlsToInitialValue(controlsPreviouslyBoundToButton, control);
            MWBase::Environment::get().getWindowManager()->notifyInputActionBound();
        }

        void setDetectingKeyboard(bool detecting)
        {
            mDetectingKeyboard = detecting;
        }

    private:
        ICS::InputControlSystem* mInputBinder;
        BindingsManager* mBindingsManager;
        bool mDetectingKeyboard;
    };

    BindingsManager::BindingsManager(const std::filesystem::path& userFile, bool userFileExists)
        : mUserFile(userFile)
        , mControlLoadedFromFile(A_Last, false)
        , mDragDrop(false)
    {
        const auto file = userFileExists ? userFile : std::filesystem::path();
        mInputBinder = std::make_unique<InputControlSystem>(file);
        mListener = std::make_unique<BindingsListener>(mInputBinder.get(), this);
        mInputBinder->setDetectingBindingListener(mListener.get());

        for (int i = 0; i < A_Last; ++i)
            mControlLoadedFromFile[i] = mInputBinder->getChannel(i)->getControlsCount() != 0;

        loadKeyDefaults();
        loadControllerDefaults();

        for (int i = 0; i < A_Last; ++i)
        {
            mInputBinder->getChannel(i)->addListener(mListener.get());
        }
    }

    void BindingsManager::setDragDrop(bool dragDrop)
    {
        mDragDrop = dragDrop;
    }

    BindingsManager::~BindingsManager()
    {
        saveBindings();
    }

    void BindingsManager::update(float dt)
    {
        // update values of channels (as a result of pressed keys)
        mInputBinder->update(dt);
    }

    bool BindingsManager::isLeftOrRightButton(int action, bool joystick) const
    {
        int mouseBinding
            = mInputBinder->getMouseButtonBinding(mInputBinder->getControl(action), ICS::Control::INCREASE);
        if (mouseBinding != ICS_MAX_DEVICE_BUTTONS)
            return true;
        int buttonBinding = mInputBinder->getJoystickButtonBinding(
            mInputBinder->getControl(action), sFakeDeviceId, ICS::Control::INCREASE);
        if (joystick && (buttonBinding == 0 || buttonBinding == 1))
            return true;
        return false;
    }

    void BindingsManager::setPlayerControlsEnabled(bool enabled)
    {
        int playerChannels[] = { A_AutoMove, A_AlwaysRun, A_ToggleWeapon, A_ToggleSpell, A_Rest, A_QuickKey1,
            A_QuickKey2, A_QuickKey3, A_QuickKey4, A_QuickKey5, A_QuickKey6, A_QuickKey7, A_QuickKey8, A_QuickKey9,
            A_QuickKey10, A_Use, A_Journal };

        for (int pc : playerChannels)
        {
            mInputBinder->getChannel(pc)->setEnabled(enabled);
        }
    }

    void BindingsManager::setJoystickDeadZone(float deadZone)
    {
        mInputBinder->setJoystickDeadZone(deadZone);
    }

    float BindingsManager::getActionValue(int id) const
    {
        return mInputBinder->getChannel(id)->getValue();
    }

    bool BindingsManager::actionIsActive(int id) const
    {
        return getActionValue(id) == 1.0;
    }

    void BindingsManager::loadKeyDefaults(bool force)
    {
        // using hardcoded key defaults is inevitable, if we want the configuration files to stay valid
        // across different versions of OpenMW (in the case where another input action is added)
        std::map<int, SDL_Scancode> defaultKeyBindings;

        // Gets the Keyvalue from the Scancode; gives the button in the same place reguardless of keyboard format
        defaultKeyBindings[A_Activate] = SDL_SCANCODE_SPACE;
        defaultKeyBindings[A_MoveBackward] = SDL_SCANCODE_S;
        defaultKeyBindings[A_MoveForward] = SDL_SCANCODE_W;
        defaultKeyBindings[A_MoveLeft] = SDL_SCANCODE_A;
        defaultKeyBindings[A_MoveRight] = SDL_SCANCODE_D;
        defaultKeyBindings[A_ToggleWeapon] = SDL_SCANCODE_F;
        defaultKeyBindings[A_ToggleSpell] = SDL_SCANCODE_R;
        defaultKeyBindings[A_CycleSpellLeft] = SDL_SCANCODE_MINUS;
        defaultKeyBindings[A_CycleSpellRight] = SDL_SCANCODE_EQUALS;
        defaultKeyBindings[A_CycleWeaponLeft] = SDL_SCANCODE_LEFTBRACKET;
        defaultKeyBindings[A_CycleWeaponRight] = SDL_SCANCODE_RIGHTBRACKET;

        defaultKeyBindings[A_QuickKeysMenu] = SDL_SCANCODE_F1;
        defaultKeyBindings[A_Console] = SDL_SCANCODE_GRAVE;
        defaultKeyBindings[A_Run] = SDL_SCANCODE_LSHIFT;
        defaultKeyBindings[A_Sneak] = SDL_SCANCODE_LCTRL;
        defaultKeyBindings[A_AutoMove] = SDL_SCANCODE_Q;
        defaultKeyBindings[A_Jump] = SDL_SCANCODE_E;
        defaultKeyBindings[A_Journal] = SDL_SCANCODE_J;
        defaultKeyBindings[A_Rest] = SDL_SCANCODE_T;
        defaultKeyBindings[A_GameMenu] = SDL_SCANCODE_ESCAPE;
        defaultKeyBindings[A_TogglePOV] = SDL_SCANCODE_TAB;
        defaultKeyBindings[A_QuickKey1] = SDL_SCANCODE_1;
        defaultKeyBindings[A_QuickKey2] = SDL_SCANCODE_2;
        defaultKeyBindings[A_QuickKey3] = SDL_SCANCODE_3;
        defaultKeyBindings[A_QuickKey4] = SDL_SCANCODE_4;
        defaultKeyBindings[A_QuickKey5] = SDL_SCANCODE_5;
        defaultKeyBindings[A_QuickKey6] = SDL_SCANCODE_6;
        defaultKeyBindings[A_QuickKey7] = SDL_SCANCODE_7;
        defaultKeyBindings[A_QuickKey8] = SDL_SCANCODE_8;
        defaultKeyBindings[A_QuickKey9] = SDL_SCANCODE_9;
        defaultKeyBindings[A_QuickKey10] = SDL_SCANCODE_0;
        defaultKeyBindings[A_Screenshot] = SDL_SCANCODE_F12;
        defaultKeyBindings[A_ToggleHUD] = SDL_SCANCODE_F11;
        defaultKeyBindings[A_ToggleDebug] = SDL_SCANCODE_F10;
        defaultKeyBindings[A_AlwaysRun] = SDL_SCANCODE_CAPSLOCK;
        defaultKeyBindings[A_QuickSave] = SDL_SCANCODE_F5;
        defaultKeyBindings[A_QuickLoad] = SDL_SCANCODE_F9;
        defaultKeyBindings[A_TogglePostProcessorHUD] = SDL_SCANCODE_F2;

        std::map<int, int> defaultMouseButtonBindings;
        defaultMouseButtonBindings[A_Inventory] = SDL_BUTTON_RIGHT;
        defaultMouseButtonBindings[A_Use] = SDL_BUTTON_LEFT;

        std::map<int, ICS::InputControlSystem::MouseWheelClick> defaultMouseWheelBindings;
        defaultMouseWheelBindings[A_ZoomIn] = ICS::InputControlSystem::MouseWheelClick::UP;
        defaultMouseWheelBindings[A_ZoomOut] = ICS::InputControlSystem::MouseWheelClick::DOWN;

        for (int i = 0; i < A_Last; ++i)
        {
            ICS::Control* control;
            bool controlExists = mInputBinder->getChannel(i)->getControlsCount() != 0;
            if (!controlExists)
            {
                control = new ICS::Control(std::to_string(i), false, true, 0, ICS::ICS_MAX, ICS::ICS_MAX);
                mInputBinder->addControl(control);
                control->attachChannel(mInputBinder->getChannel(i), ICS::Channel::DIRECT);
            }
            else
            {
                control = mInputBinder->getChannel(i)->getAttachedControls().front().control;
            }

            // Only apply defaults for actions that were missing from the loaded config file (e.g. newly added actions),
            // or when explicitly forced. Existing actions without bindings are treated as intentionally unbound.
            if (force || !mControlLoadedFromFile[static_cast<std::size_t>(i)])
            {
                clearAllKeyBindings(mInputBinder.get(), control);

                if (defaultKeyBindings.find(i) != defaultKeyBindings.end()
                    && (force || !mInputBinder->isKeyBound(defaultKeyBindings[i])))
                {
                    control->setInitialValue(0.0f);
                    mInputBinder->addKeyBinding(control, defaultKeyBindings[i], ICS::Control::INCREASE);
                }
                else if (defaultMouseButtonBindings.find(i) != defaultMouseButtonBindings.end()
                    && (force || !mInputBinder->isMouseButtonBound(defaultMouseButtonBindings[i])))
                {
                    control->setInitialValue(0.0f);
                    mInputBinder->addMouseButtonBinding(control, defaultMouseButtonBindings[i], ICS::Control::INCREASE);
                }
                else if (defaultMouseWheelBindings.find(i) != defaultMouseWheelBindings.end()
                    && (force || !mInputBinder->isMouseWheelBound(defaultMouseWheelBindings[i])))
                {
                    control->setInitialValue(0.f);
                    mInputBinder->addMouseWheelBinding(control, defaultMouseWheelBindings[i], ICS::Control::INCREASE);
                }

                if (i == A_LookLeftRight && !mInputBinder->isKeyBound(SDL_SCANCODE_KP_4)
                    && !mInputBinder->isKeyBound(SDL_SCANCODE_KP_6))
                {
                    mInputBinder->addKeyBinding(control, SDL_SCANCODE_KP_6, ICS::Control::INCREASE);
                    mInputBinder->addKeyBinding(control, SDL_SCANCODE_KP_4, ICS::Control::DECREASE);
                }
                if (i == A_LookUpDown && !mInputBinder->isKeyBound(SDL_SCANCODE_KP_8)
                    && !mInputBinder->isKeyBound(SDL_SCANCODE_KP_2))
                {
                    mInputBinder->addKeyBinding(control, SDL_SCANCODE_KP_2, ICS::Control::INCREASE);
                    mInputBinder->addKeyBinding(control, SDL_SCANCODE_KP_8, ICS::Control::DECREASE);
                }
            }
        }
    }

    void BindingsManager::loadControllerDefaults(bool force)
    {
        // using hardcoded key defaults is inevitable, if we want the configuration files to stay valid
        // across different versions of OpenMW (in the case where another input action is added)
        std::map<int, int> defaultButtonBindings;

        defaultButtonBindings[A_Activate] = SDL_CONTROLLER_BUTTON_A;
        defaultButtonBindings[A_MoveBackward] = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
        defaultButtonBindings[A_MoveForward] = SDL_CONTROLLER_BUTTON_DPAD_UP;
        defaultButtonBindings[A_MoveLeft] = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        defaultButtonBindings[A_MoveRight] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        defaultButtonBindings[A_ToggleWeapon] = SDL_CONTROLLER_BUTTON_X;
        defaultButtonBindings[A_ToggleSpell] = SDL_CONTROLLER_BUTTON_Y;
        // defaultButtonBindings[A_QuickButtonsMenu] = SDL_GetButtonFromScancode(SDL_SCANCODE_F1); // Need to implement,
        // should be ToggleSpell(5) AND Wait(9)
        defaultButtonBindings[A_Sneak] = SDL_CONTROLLER_BUTTON_LEFTSTICK;
        defaultButtonBindings[A_Journal] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
        defaultButtonBindings[A_Rest] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
        defaultButtonBindings[A_TogglePOV] = SDL_CONTROLLER_BUTTON_RIGHTSTICK;
        defaultButtonBindings[A_Inventory] = SDL_CONTROLLER_BUTTON_B;
        defaultButtonBindings[A_GameMenu] = SDL_CONTROLLER_BUTTON_START;
        defaultButtonBindings[A_QuickSave] = SDL_CONTROLLER_BUTTON_GUIDE;

        std::map<int, int> defaultAxisBindings;
        defaultAxisBindings[A_MoveForwardBackward] = SDL_CONTROLLER_AXIS_LEFTY;
        defaultAxisBindings[A_MoveLeftRight] = SDL_CONTROLLER_AXIS_LEFTX;
        defaultAxisBindings[A_LookUpDown] = SDL_CONTROLLER_AXIS_RIGHTY;
        defaultAxisBindings[A_LookLeftRight] = SDL_CONTROLLER_AXIS_RIGHTX;
        defaultAxisBindings[A_Use] = SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        defaultAxisBindings[A_Jump] = SDL_CONTROLLER_AXIS_TRIGGERLEFT;

        for (int i = 0; i < A_Last; i++)
        {
            ICS::Control* control;
            bool controlExists = mInputBinder->getChannel(i)->getControlsCount() != 0;
            if (!controlExists)
            {
                float initial;
                if (defaultAxisBindings.find(i) == defaultAxisBindings.end())
                    initial = 0.0f;
                else
                    initial = 0.5f;
                control = new ICS::Control(std::to_string(i), false, true, initial, ICS::ICS_MAX, ICS::ICS_MAX);
                mInputBinder->addControl(control);
                control->attachChannel(mInputBinder->getChannel(i), ICS::Channel::DIRECT);
            }
            else
            {
                control = mInputBinder->getChannel(i)->getAttachedControls().front().control;
            }

            const bool controlWasLoadedFromFile = mControlLoadedFromFile[static_cast<std::size_t>(i)];
            const bool isHiddenAxisAction
                = (i == A_LookUpDown || i == A_LookLeftRight || i == A_MoveForwardBackward || i == A_MoveLeftRight);
            const bool hasControllerBinding
                = mInputBinder->getJoystickAxisBinding(control, sFakeDeviceId, ICS::Control::INCREASE)
                    != ICS::InputControlSystem::UNASSIGNED
                || mInputBinder->getJoystickAxisBinding(control, sFakeDeviceId, ICS::Control::DECREASE)
                    != ICS::InputControlSystem::UNASSIGNED
                || mInputBinder->getJoystickButtonBinding(control, sFakeDeviceId, ICS::Control::INCREASE)
                    != ICS_MAX_DEVICE_BUTTONS
                || mInputBinder->getJoystickButtonBinding(control, sFakeDeviceId, ICS::Control::DECREASE)
                    != ICS_MAX_DEVICE_BUTTONS;

            // Only apply defaults for actions that were missing from the loaded config file (e.g. newly added actions),
            // or when explicitly forced. Existing actions without bindings are treated as intentionally unbound.
            //
            // Hidden joystick axes (movement/look) are not exposed in the UI, so make sure they always have bindings.
            if (force || !controlWasLoadedFromFile || (isHiddenAxisAction && !hasControllerBinding))
            {
                clearAllControllerBindings(mInputBinder.get(), control);

                if (defaultButtonBindings.find(i) != defaultButtonBindings.end()
                    && (force || !mInputBinder->isJoystickButtonBound(sFakeDeviceId, defaultButtonBindings[i])))
                {
                    control->setInitialValue(0.0f);
                    mInputBinder->addJoystickButtonBinding(
                        control, sFakeDeviceId, defaultButtonBindings[i], ICS::Control::INCREASE);
                }
                else if (defaultAxisBindings.find(i) != defaultAxisBindings.end()
                    && (force || !mInputBinder->isJoystickAxisBound(sFakeDeviceId, defaultAxisBindings[i])))
                {
                    control->setValue(0.5f);
                    control->setInitialValue(0.5f);
                    mInputBinder->addJoystickAxisBinding(
                        control, sFakeDeviceId, defaultAxisBindings[i], ICS::Control::INCREASE);
                }
            }
        }
    }

    std::string_view BindingsManager::getActionDescription(int action)
    {
        switch (action)
        {
            case A_Screenshot:
                return "#{OMWEngine:Screenshot}";
            case A_ZoomIn:
                return "#{OMWEngine:CameraZoomIn}";
            case A_ZoomOut:
                return "#{OMWEngine:CameraZoomOut}";
            case A_ToggleHUD:
                return "#{OMWEngine:ToggleHUD}";
            case A_Use:
                return "#{sUse}";
            case A_Activate:
                return "#{sActivate}";
            case A_MoveBackward:
                return "#{sBack}";
            case A_MoveForward:
                return "#{sForward}";
            case A_MoveLeft:
                return "#{sLeft}";
            case A_MoveRight:
                return "#{sRight}";
            case A_ToggleWeapon:
                return "#{sReady_Weapon}";
            case A_ToggleSpell:
                return "#{sReady_Magic}";
            case A_CycleSpellLeft:
                return "#{sPrevSpell}";
            case A_CycleSpellRight:
                return "#{sNextSpell}";
            case A_CycleWeaponLeft:
                return "#{sPrevWeapon}";
            case A_CycleWeaponRight:
                return "#{sNextWeapon}";
            case A_Console:
                return "#{OMWEngine:ConsoleWindow}";
            case A_Run:
                return "#{sRun}";
            case A_Sneak:
                return "#{sCrouch_Sneak}";
            case A_AutoMove:
                return "#{sAuto_Run}";
            case A_Jump:
                return "#{sJump}";
            case A_Journal:
                return "#{sJournal}";
            case A_Rest:
                return "#{sRestKey}";
            case A_Inventory:
                return "#{sInventory}";
            case A_TogglePOV:
                return "#{sTogglePOVCmd}";
            case A_QuickKeysMenu:
                return "#{sQuickMenu}";
            case A_QuickKey1:
                return "#{sQuick1Cmd}";
            case A_QuickKey2:
                return "#{sQuick2Cmd}";
            case A_QuickKey3:
                return "#{sQuick3Cmd}";
            case A_QuickKey4:
                return "#{sQuick4Cmd}";
            case A_QuickKey5:
                return "#{sQuick5Cmd}";
            case A_QuickKey6:
                return "#{sQuick6Cmd}";
            case A_QuickKey7:
                return "#{sQuick7Cmd}";
            case A_QuickKey8:
                return "#{sQuick8Cmd}";
            case A_QuickKey9:
                return "#{sQuick9Cmd}";
            case A_QuickKey10:
                return "#{sQuick10Cmd}";
            case A_AlwaysRun:
                return "#{sAlways_Run}";
            case A_QuickSave:
                return "#{sQuickSaveCmd}";
            case A_QuickLoad:
                return "#{sQuickLoadCmd}";
            case A_TogglePostProcessorHUD:
                return "#{OMWEngine:TogglePostProcessorHUD}";
            default:
                return {}; // not configurable
        }
    }

    std::string BindingsManager::getActionKeyBindingName(int action)
    {
        if (mInputBinder->getChannel(action)->getControlsCount() == 0)
            return "#{Interface:None}";

        ICS::Control* c = mInputBinder->getChannel(action)->getAttachedControls().front().control;

        SDL_Scancode key = mInputBinder->getKeyBinding(c, ICS::Control::INCREASE);
        unsigned int mouse = mInputBinder->getMouseButtonBinding(c, ICS::Control::INCREASE);
        ICS::InputControlSystem::MouseWheelClick wheel = mInputBinder->getMouseWheelBinding(c, ICS::Control::INCREASE);
        if (key != SDL_SCANCODE_UNKNOWN)
            return MyGUI::TextIterator::toTagsString(mInputBinder->scancodeToString(key));
        else if (mouse != ICS_MAX_DEVICE_BUTTONS)
            return "#{sMouse} " + std::to_string(mouse);
        else if (wheel != ICS::InputControlSystem::MouseWheelClick::UNASSIGNED)
            switch (wheel)
            {
                case ICS::InputControlSystem::MouseWheelClick::UP:
                    return "Mouse Wheel Up";
                case ICS::InputControlSystem::MouseWheelClick::DOWN:
                    return "Mouse Wheel Down";
                case ICS::InputControlSystem::MouseWheelClick::RIGHT:
                    return "Mouse Wheel Right";
                case ICS::InputControlSystem::MouseWheelClick::LEFT:
                    return "Mouse Wheel Left";
                default:
                    return "#{Interface:None}";
            }
        else
            return "#{Interface:None}";
    }

    std::string BindingsManager::getActionControllerBindingName(int action)
    {
        if (mInputBinder->getChannel(action)->getControlsCount() == 0)
            return "#{Interface:None}";

        ICS::Control* c = mInputBinder->getChannel(action)->getAttachedControls().front().control;

        if (mInputBinder->getJoystickAxisBinding(c, sFakeDeviceId, ICS::Control::INCREASE)
            != ICS::InputControlSystem::UNASSIGNED)
            return SDLUtil::sdlControllerAxisToString(
                mInputBinder->getJoystickAxisBinding(c, sFakeDeviceId, ICS::Control::INCREASE));
        else if (mInputBinder->getJoystickButtonBinding(c, sFakeDeviceId, ICS::Control::INCREASE)
            != ICS_MAX_DEVICE_BUTTONS)
            return SDLUtil::sdlControllerButtonToString(
                mInputBinder->getJoystickButtonBinding(c, sFakeDeviceId, ICS::Control::INCREASE));
        else
            return "#{Interface:None}";
    }

    const std::initializer_list<int>& BindingsManager::getActionKeySorting()
    {
        static const std::initializer_list<int> actions{ A_MoveForward, A_MoveBackward, A_MoveLeft, A_MoveRight,
            A_TogglePOV, A_ZoomIn, A_ZoomOut, A_Run, A_AlwaysRun, A_Sneak, A_Activate, A_Use, A_ToggleWeapon,
            A_ToggleSpell, A_CycleSpellLeft, A_CycleSpellRight, A_CycleWeaponLeft, A_CycleWeaponRight, A_AutoMove,
            A_Jump, A_Inventory, A_Journal, A_Rest, A_Console, A_QuickSave, A_QuickLoad, A_ToggleHUD, A_Screenshot,
            A_QuickKeysMenu, A_QuickKey1, A_QuickKey2, A_QuickKey3, A_QuickKey4, A_QuickKey5, A_QuickKey6, A_QuickKey7,
            A_QuickKey8, A_QuickKey9, A_QuickKey10, A_TogglePostProcessorHUD };

        return actions;
    }
    const std::initializer_list<int>& BindingsManager::getActionControllerSorting()
    {
        static const std::initializer_list<int> actions{ A_MoveForward, A_MoveBackward, A_MoveLeft, A_MoveRight,
            A_TogglePOV, A_ZoomIn, A_ZoomOut, A_AlwaysRun, A_Sneak, A_Activate, A_Use, A_ToggleWeapon, A_ToggleSpell,
            A_AutoMove, A_Jump, A_Inventory, A_Journal, A_Rest, A_QuickSave, A_QuickLoad, A_ToggleHUD, A_Screenshot,
            A_QuickKeysMenu, A_QuickKey1, A_QuickKey2, A_QuickKey3, A_QuickKey4, A_QuickKey5, A_QuickKey6, A_QuickKey7,
            A_QuickKey8, A_QuickKey9, A_QuickKey10, A_CycleSpellLeft, A_CycleSpellRight, A_CycleWeaponLeft,
            A_CycleWeaponRight };

        return actions;
    }

    void BindingsManager::enableDetectingBindingMode(int action, bool keyboard)
    {
        mListener->setDetectingKeyboard(keyboard);
        ICS::Control* c = mInputBinder->getChannel(action)->getAttachedControls().front().control;
        mInputBinder->enableDetectingBindingState(c, ICS::Control::INCREASE);
    }

    bool BindingsManager::isDetectingBindingState() const
    {
        return mInputBinder->detectingBindingState();
    }

    void BindingsManager::mousePressed(const SDL_MouseButtonEvent& arg, Uint8 deviceID)
    {
        mInputBinder->mousePressed(arg, deviceID);
    }

    void BindingsManager::mouseReleased(const SDL_MouseButtonEvent& arg, Uint8 deviceID)
    {
        mInputBinder->mouseReleased(arg, deviceID);
    }

    void BindingsManager::mouseMoved(const SDLUtil::MouseMotionEvent& arg)
    {
        mInputBinder->mouseMoved(arg);
    }

    void BindingsManager::mouseWheelMoved(const SDL_MouseWheelEvent& arg)
    {
        mInputBinder->mouseWheelMoved(arg);
    }

    void BindingsManager::keyPressed(const SDL_KeyboardEvent& arg)
    {
        mInputBinder->keyPressed(arg);
    }

    void BindingsManager::keyReleased(const SDL_KeyboardEvent& arg)
    {
        mInputBinder->keyReleased(arg);
    }

    void BindingsManager::controllerAdded(int deviceID, const SDL_ControllerDeviceEvent& arg)
    {
        mInputBinder->controllerAdded(deviceID, arg);
    }

    void BindingsManager::controllerRemoved(const SDL_ControllerDeviceEvent& arg)
    {
        mInputBinder->controllerRemoved(arg);
    }

    void BindingsManager::controllerButtonPressed(int deviceID, const SDL_ControllerButtonEvent& arg)
    {
        mInputBinder->buttonPressed(deviceID, arg);
    }

    void BindingsManager::controllerButtonReleased(int deviceID, const SDL_ControllerButtonEvent& arg)
    {
        mInputBinder->buttonReleased(deviceID, arg);
    }

    void BindingsManager::controllerAxisMoved(int deviceID, const SDL_ControllerAxisEvent& arg)
    {
        constexpr float triggerPressThreshold = 0.6f;
        constexpr float triggerReleaseThreshold = 0.55f;

        auto normalizeTriggerValue = [](Sint16 value) -> float {
            // SDL controller triggers are reported as axes in the range [0..32767]. Our input system normalizes
            // axes from [-32768..32767] into [0..1], which makes the trigger "rest" position 0.5.
            constexpr float min = -32768.f;
            constexpr float range = 65535.f;
            const float normalized = (static_cast<float>(value) - min) / range;
            return std::clamp(normalized, 0.f, 1.f);
        };

        const bool triggerAxis
            = arg.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || arg.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        if (triggerAxis)
        {
            auto inputManager = MWBase::Environment::get().getInputManager();
            const bool joystickUsed
                = inputManager->joystickLastUsed() && inputManager->getControlSwitch("playercontrols");

            const bool toggleWeapon = actionIsActive(A_ToggleWeapon);
            const bool toggleSpell = actionIsActive(A_ToggleSpell);
            const bool chordActive = joystickUsed && (toggleWeapon || toggleSpell);

            const float norm = normalizeTriggerValue(arg.value);
            const bool pressed = norm > triggerPressThreshold;
            const bool released = norm < triggerReleaseThreshold;

            if (arg.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
            {
                if (chordActive && pressed && !mLeftTriggerChordPressed)
                {
                    const int cycleAction = toggleWeapon ? A_CycleWeaponLeft : A_CycleSpellLeft;
                    inputManager->executeAction(cycleAction);
                    if (toggleWeapon)
                        mToggleWeaponChorded = true;
                    else
                        mToggleSpellChorded = true;
                }

                mLeftTriggerChordPressed = pressed;
                if (chordActive && pressed)
                    mIgnoreLeftTriggerUntilRelease = true;
                if (released)
                    mIgnoreLeftTriggerUntilRelease = false;

                if (chordActive || mIgnoreLeftTriggerUntilRelease)
                {
                    if (!released)
                        return;
                }
            }
            else // SDL_CONTROLLER_AXIS_TRIGGERRIGHT
            {
                if (chordActive && pressed && !mRightTriggerChordPressed)
                {
                    const int cycleAction = toggleWeapon ? A_CycleWeaponRight : A_CycleSpellRight;
                    inputManager->executeAction(cycleAction);
                    if (toggleWeapon)
                        mToggleWeaponChorded = true;
                    else
                        mToggleSpellChorded = true;
                }

                mRightTriggerChordPressed = pressed;
                if (chordActive && pressed)
                    mIgnoreRightTriggerUntilRelease = true;
                if (released)
                    mIgnoreRightTriggerUntilRelease = false;

                if (chordActive || mIgnoreRightTriggerUntilRelease)
                {
                    if (!released)
                        return;
                }
            }
        }

        mInputBinder->axisMoved(deviceID, arg);
    }

    SDL_Scancode BindingsManager::getKeyBinding(int actionId)
    {
        return mInputBinder->getKeyBinding(mInputBinder->getControl(actionId), ICS::Control::INCREASE);
    }

    SDL_GameController* BindingsManager::getControllerOrNull() const
    {
        const auto& controllers = mInputBinder->getJoystickInstanceMap();
        if (controllers.empty())
            return nullptr;
        else
            return controllers.begin()->second;
    }

    void BindingsManager::actionValueChanged(int action, float currentValue, float previousValue)
    {
        if (isDetectingBindingState())
            return;

        auto manager = MWBase::Environment::get().getInputManager();
        manager->resetIdleTime();

        if (mDragDrop && action != A_GameMenu && action != A_Inventory)
            return;

        const bool joystickUsed = manager->joystickLastUsed() && manager->getControlSwitch("playercontrols");
        if (joystickUsed)
        {
            auto isActionBoundToAxis = [this](int actionId, int axis) -> bool {
                auto* channel = mInputBinder->getChannel(actionId);
                if (channel->getControlsCount() == 0)
                    return false;

                for (const auto& attached : channel->getAttachedControls())
                {
                    ICS::Control* control = attached.control;
                    if (mInputBinder->getJoystickAxisBinding(control, sFakeDeviceId, ICS::Control::INCREASE) == axis)
                        return true;
                    if (mInputBinder->getJoystickAxisBinding(control, sFakeDeviceId, ICS::Control::DECREASE) == axis)
                        return true;
                }

                return false;
            };

            if (previousValue <= 0.6f && currentValue > 0.6f)
            {
                // If a trigger press was used as a chord (X/Y + LT/RT), suppress the normal action bound to that
                // trigger so it doesn't fire (e.g. jump/attack).
                if (mIgnoreLeftTriggerUntilRelease && isActionBoundToAxis(action, SDL_CONTROLLER_AXIS_TRIGGERLEFT))
                    return;
                if (mIgnoreRightTriggerUntilRelease && isActionBoundToAxis(action, SDL_CONTROLLER_AXIS_TRIGGERRIGHT))
                    return;
            }

            if (action == A_ToggleWeapon || action == A_ToggleSpell)
            {
                // Let chorded inputs win: toggle only on release if no chord fired.
                if (previousValue <= 0.6f && currentValue > 0.6f)
                {
                    if (action == A_ToggleWeapon)
                        mToggleWeaponChorded = false;
                    else
                        mToggleSpellChorded = false;
                    return;
                }
                if (previousValue > 0.6f && currentValue <= 0.6f)
                {
                    const bool chorded = action == A_ToggleWeapon ? mToggleWeaponChorded : mToggleSpellChorded;
                    if (action == A_ToggleWeapon)
                        mToggleWeaponChorded = false;
                    else
                        mToggleSpellChorded = false;
                    if (!chorded)
                        manager->executeAction(action);
                    return;
                }
            }

            // Cycling weapon/spell while holding toggle is bound to the physical controller triggers (LT/RT)
            // and is handled in controllerAxisMoved. Do not derive this chord from configurable actions like Jump/Use.
        }

        if (previousValue <= 0.6 && currentValue > 0.6)
            manager->executeAction(action);
    }

    void BindingsManager::saveBindings()
    {
        std::string newFileName;
        try
        {
            newFileName = Files::pathToUnicodeString(mUserFile) + ".new";
            if (mInputBinder->save(newFileName))
            {
                std::filesystem::rename(Files::pathFromUnicodeString(newFileName), mUserFile);
                Log(Debug::Info) << "Saved input bindings: " << mUserFile;
            }
            else
            {
                Log(Debug::Error) << "Failed to save input bindings to " << newFileName;
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "Failed to save input bindings to " << newFileName << ": " << e.what();
        }
    }
}
