#include "inputmanagerimp.hpp"

#include <string>
#include <string_view>

#include <osgViewer/ViewerEventHandlers>

#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/sdlutil/sdlinputwrapper.hpp>
#include <components/settings/values.hpp>

#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_PointerManager.h>
#include <MyGUI_Widget.h>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/esmstore.hpp"

#include "actionmanager.hpp"
#include "bindingsmanager.hpp"
#include "controllermanager.hpp"
#include "controlswitch.hpp"
#include "gyromanager.hpp"
#include "keyboardmanager.hpp"
#include "mousemanager.hpp"
#include "sensormanager.hpp"

namespace MWInput
{
    namespace
    {
        std::string sSoftwareCursorPointer = "arrow";
        bool sSoftwareCursorHookedPointerChange = false;

        void onSoftwareCursorPointerChanged(std::string_view name)
        {
            sSoftwareCursorPointer.assign(name.data(), name.size());
        }

        void ensureSoftwareCursorTracksPointer()
        {
            if (sSoftwareCursorHookedPointerChange)
                return;
            sSoftwareCursorHookedPointerChange = true;

            MyGUI::PointerManager::getInstance().eventChangeMousePointer
                += MyGUI::newDelegate(&onSoftwareCursorPointerChanged);

            sSoftwareCursorPointer = MyGUI::PointerManager::getInstance().getDefaultPointer();
        }

        struct SoftwareCursorStyle
        {
            const char* mTexture;
            int mHotspotX;
            int mHotspotY;
        };

        int scaleHotspot(int sourceHotspot, int renderSize, int sourceSize)
        {
            return (sourceHotspot * renderSize + sourceSize / 2) / sourceSize;
        }

        SoftwareCursorStyle getSoftwareCursorStyle(std::string_view pointerName)
        {
            if (pointerName == "hresize")
                return { "textures\\H-RESIZE.dds", 16, 14 };
            if (pointerName == "vresize")
                return { "textures\\V-RESIZE.dds", 17, 16 };
            if (pointerName == "dresize" || pointerName == "dresize2")
                return { "textures\\H-RESIZE.dds", 16, 14 };

            // Fall back to normal cursor for unknown pointers.
            return { "textures\\tx_cursor.dds", 7, 0 };
        }

        bool ensureWaylandSoftwareCursor(MyGUI::ImageBox*& cursor)
        {
            if (cursor != nullptr)
                return true;

            if (MyGUI::Gui::getInstancePtr() == nullptr)
                return false;

            ensureSoftwareCursorTracksPointer();

            constexpr int sourceWidth = 32;
            constexpr int sourceHeight = 32;
            constexpr int renderWidth = (sourceWidth * 5) / 8;
            constexpr int renderHeight = (sourceHeight * 5) / 8;

            cursor = MyGUI::Gui::getInstance().createWidget<MyGUI::ImageBox>(
                "ImageBox", 0, 0, renderWidth, renderHeight, MyGUI::Align::Default, "Pointer");
            cursor->setImageTexture("textures\\tx_cursor.dds");
            cursor->setUserString("SoftwareCursorTexture", "textures\\tx_cursor.dds");
            cursor->setImageCoord(MyGUI::IntCoord(0, 0, sourceWidth, sourceHeight));
            cursor->setNeedMouseFocus(false);
            cursor->setNeedKeyFocus(false);
            cursor->setVisible(false);
            return true;
        }

        void updateWaylandSoftwareCursor(MyGUI::ImageBox* cursor, bool visible)
        {
            if (cursor == nullptr)
                return;

            cursor->setVisible(visible);
            if (!visible)
                return;

            constexpr int sourceWidth = 32;
            constexpr int sourceHeight = 32;
            constexpr int cursorWidth = (sourceWidth * 5) / 8;
            constexpr int cursorHeight = (sourceHeight * 5) / 8;

            std::string_view pointerName = sSoftwareCursorPointer;
            if (const MyGUI::Widget* widget = MyGUI::InputManager::getInstance().getMouseFocusWidget())
            {
                const std::string& widgetPointer = widget->getPointer();
                if (!widgetPointer.empty())
                    pointerName = widgetPointer;
            }

            const SoftwareCursorStyle style = getSoftwareCursorStyle(pointerName);
            if (cursor->getUserString("SoftwareCursorTexture") != style.mTexture)
            {
                cursor->setImageTexture(style.mTexture);
                cursor->setUserString("SoftwareCursorTexture", style.mTexture);
            }

            const int hotspotX = scaleHotspot(style.mHotspotX, cursorWidth, sourceWidth);
            const int hotspotY = scaleHotspot(style.mHotspotY, cursorHeight, sourceHeight);

            const MyGUI::IntPoint pos = MyGUI::InputManager::getInstance().getMousePosition();
            cursor->setCoord(pos.left - hotspotX, pos.top - hotspotY, cursorWidth, cursorHeight);
        }
    }

    InputManager::InputManager(SDL_Window* window, osg::ref_ptr<osgViewer::Viewer> viewer,
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> screenCaptureHandler, const std::filesystem::path& userFile,
        bool userFileExists, const std::filesystem::path& userControllerBindingsFile,
        const std::filesystem::path& controllerBindingsFile, bool grab)
        : mControlsDisabled(false)
        , mInputWrapper(std::make_unique<SDLUtil::InputWrapper>(window, viewer, grab))
        , mBindingsManager(std::make_unique<BindingsManager>(userFile, userFileExists))
        , mControlSwitch(std::make_unique<ControlSwitch>())
        , mActionManager(std::make_unique<ActionManager>(mBindingsManager.get(), viewer, screenCaptureHandler))
        , mKeyboardManager(std::make_unique<KeyboardManager>(mBindingsManager.get()))
        , mMouseManager(std::make_unique<MouseManager>(mBindingsManager.get(), mInputWrapper.get(), window))
        , mControllerManager(std::make_unique<ControllerManager>(
              mBindingsManager.get(), mMouseManager.get(), userControllerBindingsFile, controllerBindingsFile))
        , mSensorManager(std::make_unique<SensorManager>())
        , mGyroManager(std::make_unique<GyroManager>())
    {
        mInputWrapper->setWindowEventCallback(MWBase::Environment::get().getWindowManager());
        mInputWrapper->setKeyboardEventCallback(mKeyboardManager.get());
        mInputWrapper->setMouseEventCallback(mMouseManager.get());
        mInputWrapper->setControllerEventCallback(mControllerManager.get());
        mInputWrapper->setSensorEventCallback(mSensorManager.get());
    }

    void InputManager::clear()
    {
        // Enable all controls
        mControlSwitch->clear();
    }

    InputManager::~InputManager()
    {
        if (mWaylandSoftwareCursor != nullptr && MyGUI::Gui::getInstancePtr() != nullptr)
            MyGUI::Gui::getInstance().destroyWidget(mWaylandSoftwareCursor);
        mWaylandSoftwareCursor = nullptr;
    }

    void InputManager::update(float dt, bool disableControls, bool disableEvents)
    {
        mControlsDisabled = disableControls;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        const bool cursorVisible = winMgr->getCursorVisible();
        const bool wantSoftwareCursor = Settings::input().mEnableSoftwareMouse && Settings::gui().mControllerMenus
            && cursorVisible && mControllerManager->joystickLastUsed();
        const bool controllerCursorActive = wantSoftwareCursor && ensureWaylandSoftwareCursor(mWaylandSoftwareCursor);

        mInputWrapper->setMouseVisible(cursorVisible && !controllerCursorActive);
        mInputWrapper->capture(disableEvents);

        if (disableControls)
        {
            mMouseManager->updateCursorMode();
            mInputWrapper->setMouseVisible(cursorVisible && !controllerCursorActive);
            updateWaylandSoftwareCursor(mWaylandSoftwareCursor, controllerCursorActive);
            return;
        }

        mBindingsManager->update(dt);

        mMouseManager->updateCursorMode();

        mControllerManager->update(dt);
        mMouseManager->update(dt);
        mSensorManager->update(dt);
        mActionManager->update(dt);

        if (Settings::input().mEnableGyroscope)
        {
            bool controllerAvailable = mControllerManager->isGyroAvailable();
            bool sensorAvailable = mSensorManager->isGyroAvailable();
            if (controllerAvailable || sensorAvailable)
            {
                mGyroManager->update(
                    dt, controllerAvailable ? mControllerManager->getGyroValues() : mSensorManager->getGyroValues());
            }
        }

        mInputWrapper->setMouseVisible(cursorVisible && !controllerCursorActive);
        updateWaylandSoftwareCursor(mWaylandSoftwareCursor, controllerCursorActive);
    }

    void InputManager::setDragDrop(bool dragDrop)
    {
        mBindingsManager->setDragDrop(dragDrop);
    }

    void InputManager::setGamepadGuiCursorEnabled(bool enabled)
    {
        mControllerManager->setGamepadGuiCursorEnabled(enabled);
    }

    bool InputManager::isGamepadGuiCursorEnabled()
    {
        return mControllerManager->gamepadGuiCursorEnabled();
    }

    void InputManager::changeInputMode(bool guiMode)
    {
        mControllerManager->setGuiCursorEnabled(guiMode);
        mMouseManager->setGuiCursorEnabled(guiMode);
        mGyroManager->setGuiCursorEnabled(guiMode);
        mMouseManager->setMouseLookEnabled(!guiMode);
        if (guiMode)
            MWBase::Environment::get().getWindowManager()->showCrosshair(false);

        bool isCursorVisible
            = guiMode && (!mControllerManager->joystickLastUsed() || mControllerManager->gamepadGuiCursorEnabled());
        MWBase::Environment::get().getWindowManager()->setCursorVisible(isCursorVisible);
        // if not in gui mode, the camera decides whether to show crosshair or not.
    }

    void InputManager::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        mSensorManager->processChangedSettings(changed);
    }

    bool InputManager::getControlSwitch(std::string_view sw)
    {
        return mControlSwitch->get(sw);
    }

    void InputManager::toggleControlSwitch(std::string_view sw, bool value)
    {
        mControlSwitch->set(sw, value);
    }

    void InputManager::resetIdleTime()
    {
        mActionManager->resetIdleTime();
    }

    bool InputManager::isIdle() const
    {
        return mActionManager->getIdleTime() > 0.5;
    }

    std::string_view InputManager::getActionDescription(int action) const
    {
        return mBindingsManager->getActionDescription(action);
    }

    std::string InputManager::getActionKeyBindingName(int action) const
    {
        return mBindingsManager->getActionKeyBindingName(action);
    }

    std::string InputManager::getActionControllerBindingName(int action) const
    {
        return mBindingsManager->getActionControllerBindingName(action);
    }

    bool InputManager::actionIsActive(int action) const
    {
        return mBindingsManager->actionIsActive(action);
    }

    float InputManager::getActionValue(int action) const
    {
        return mBindingsManager->getActionValue(action);
    }

    bool InputManager::isControllerButtonPressed(SDL_GameControllerButton button) const
    {
        return mControllerManager->isButtonPressed(button);
    }

    float InputManager::getControllerAxisValue(SDL_GameControllerAxis axis) const
    {
        return mControllerManager->getAxisValue(axis);
    }

    int InputManager::getMouseMoveX() const
    {
        return mMouseManager->getMouseMoveX();
    }

    int InputManager::getMouseMoveY() const
    {
        return mMouseManager->getMouseMoveY();
    }

    void InputManager::warpMouseToWidget(MyGUI::Widget* widget)
    {
        // This is currently used to simulate mouse movement when the gamepad UI is used.
        // Sometimes this is called in reaction to layout changes.
        // It's a bad idea to do this if the user triggered one with the actual mouse.

        // Don't warp if a gamepad wasn't in use when this was triggered.
        if (!joystickLastUsed())
            return;

        // Don't warp if the mouse button is actively being held.
        // TODO: this should be a method somewhere so that it can be reused in, e.g., Lua bindings
        if (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK)
            return;

        // Don't warp if an emulated mouse press is occurring.
        if (isGamepadGuiCursorEnabled() && isControllerButtonPressed(SDL_CONTROLLER_BUTTON_A))
            return;

        MWBase::Environment::get().getWindowManager()->setCursorVisible(false);
        mMouseManager->warpMouseToWidget(widget);
        mMouseManager->injectMouseMove(1, 0, 0);
        MWBase::Environment::get().getWindowManager()->setCursorActive(true);
    }

    const std::initializer_list<int>& InputManager::getActionKeySorting()
    {
        return mBindingsManager->getActionKeySorting();
    }

    const std::initializer_list<int>& InputManager::getActionControllerSorting()
    {
        return mBindingsManager->getActionControllerSorting();
    }

    void InputManager::enableDetectingBindingMode(int action, bool keyboard)
    {
        mBindingsManager->enableDetectingBindingMode(action, keyboard);
    }

    size_t InputManager::countSavedGameRecords() const
    {
        return mControlSwitch->countSavedGameRecords();
    }

    void InputManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
    {
        mControlSwitch->write(writer, progress);
    }

    void InputManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type == ESM::REC_INPU)
        {
            mControlSwitch->readRecord(reader, type);
        }
    }

    void InputManager::resetToDefaultKeyBindings()
    {
        mBindingsManager->loadKeyDefaults(true);
    }

    void InputManager::resetToDefaultControllerBindings()
    {
        mBindingsManager->loadControllerDefaults(true);
    }

    void InputManager::setJoystickLastUsed(bool enabled)
    {
        mControllerManager->setJoystickLastUsed(enabled);
    }

    bool InputManager::joystickLastUsed()
    {
        return mControllerManager->joystickLastUsed();
    }

    std::string InputManager::getControllerButtonIcon(int button)
    {
        return mControllerManager->getControllerButtonIcon(button);
    }

    std::string InputManager::getControllerAxisIcon(int axis)
    {
        return mControllerManager->getControllerAxisIcon(axis);
    }

    void InputManager::executeAction(int action)
    {
        mActionManager->executeAction(action);
    }

    void InputManager::saveBindings()
    {
        mBindingsManager->saveBindings();
    }
}
