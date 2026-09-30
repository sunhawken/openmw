#ifndef MWGUI_SETTINGS_H
#define MWGUI_SETTINGS_H

#include <components/files/configurationmanager.hpp>
#include <components/lua_ui/adapter.hpp>

#include "windowbase.hpp"

namespace osg
{
    class Group;
}
namespace Resource
{
    class ResourceSystem;
}

namespace MWGui
{
    class SettingsWindow : public WindowBase
    {
    public:
        SettingsWindow(Files::ConfigurationManager& cfgMgr, osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem);
        ~SettingsWindow() override;

        void onOpen() override;

        void onClose() override;

        void onFrame(float duration) override;

        void updateControlsBox();

        void updateLightSettings();

        void updateVSyncModeSettings();

        void updateWindowModeSettings();

        void onResChange(int, int) override;

        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;

    protected:
        MyGUI::TabControl* mSettingsTab;
        MyGUI::Button* mOkButton;

        // graphics
        MyGUI::ListBox* mResolutionList;
        MyGUI::ComboBox* mWindowModeList;
        MyGUI::ComboBox* mVSyncModeList;
        MyGUI::Button* mWindowBorderButton;
        MyGUI::ComboBox* mTextureFilteringButton;

        MyGUI::Button* mWaterRefractionButton;
        MyGUI::Button* mSunlightScatteringButton;
        MyGUI::Button* mWobblyShoresButton;
        MyGUI::ComboBox* mWaterTextureSize;
        MyGUI::ComboBox* mWaterReflectionDetail;
        MyGUI::ComboBox* mWaterRainRippleDetail;

        MyGUI::ComboBox* mMaxLights;
        MyGUI::ComboBox* mLightingMethodButton;
        MyGUI::Button* mLightsResetButton;
        MyGUI::Button* mJiggleOffsetResetButton;
        MyGUI::Button* mVerletResetButton;
        MyGUI::Button* mBakeBreastToNifButton;
        MyGUI::Button* mJiggleAdvancedPanelToggle;
        std::unique_ptr<Layout> mJiggleAdvancedLayout;
        std::unique_ptr<class JiggleRetargetPanel> mJiggleRetargetPanel;
        MyGUI::Button* mJiggleRetargetButton;
        void onJiggleRetargetClicked(MyGUI::Widget* sender);
        osg::Group* mSceneRoot;
        Resource::ResourceSystem* mResourceSystem;
        MyGUI::Window* mJiggleAdvancedWindow;
        MyGUI::TextBox* mJiggleQuickCurrentInfo;
        MyGUI::Button* mJiggleQuickAddAnyButton;
        MyGUI::Button* mJiggleQuickAddPlayerButton;
        MyGUI::Button* mJiggleQuickBlacklistButton;
        MyGUI::Button* mJiggleQuickClearButton;
        MyGUI::Button* mJiggleAdvancedCloseButton;
        MyGUI::ComboBox* mJiggleMeshScopeCombo;
        MyGUI::EditBox* mJiggleMeshPathInput;
        MyGUI::EditBox* mJiggleNpcNameInput;
        MyGUI::EditBox* mJiggleBreastOffsetInput;
        MyGUI::EditBox* mJiggleButtOffsetInput;
        MyGUI::ListBox* mJiggleMeshOffsetList;
        MyGUI::ListBox* mJiggleBlacklistList;
        MyGUI::ListBox* mJiggleNpcRuleList;
        MyGUI::Button* mJiggleUseCurrentMeshButton;
        MyGUI::Button* mJiggleSaveMeshOffsetButton;
        MyGUI::Button* mJiggleRemoveMeshOffsetButton;
        MyGUI::Button* mJiggleAddBlacklistButton;
        MyGUI::Button* mJiggleRemoveBlacklistButton;
        MyGUI::Button* mJiggleEnableNpcButton;
        MyGUI::Button* mJiggleDisableNpcButton;
        MyGUI::Button* mJiggleClearNpcRuleButton;

        MyGUI::ComboBox* mShadowResolution;
        MyGUI::ComboBox* mShadowUpdateInterval;

        MyGUI::ComboBox* mPrimaryLanguage;
        MyGUI::ComboBox* mSecondaryLanguage;
        MyGUI::Button* mGmstOverridesL10n;

        MyGUI::Widget* mWindowModeHint;

        // controls
        MyGUI::ScrollView* mControlsBox;
        MyGUI::Button* mResetControlsButton;
        MyGUI::Button* mKeyboardSwitch;
        MyGUI::Button* mControllerSwitch;
        bool mKeyboardMode; // if true, setting up the keyboard. Otherwise, it's controller

        MyGUI::EditBox* mScriptFilter;
        MyGUI::ListBox* mScriptList;
        MyGUI::Widget* mScriptBox;
        MyGUI::Widget* mScriptDisabled;
        MyGUI::ScrollView* mScriptView;
        LuaUi::LuaAdapter* mScriptAdapter;
        size_t mCurrentPage;

        void onTabChanged(MyGUI::TabControl* sender, size_t index);
        void onOkButtonClicked(MyGUI::Widget* sender);
        void onTextureFilteringChanged(MyGUI::ComboBox* sender, size_t pos);
        void onSliderChangePosition(MyGUI::ScrollBar* scroller, size_t pos);
        void onButtonToggled(MyGUI::Widget* sender);
        void onResolutionSelected(MyGUI::ListBox* sender, size_t index);
        void onResolutionAccept();
        void onResolutionCancel();
        void highlightCurrentResolution();

        void onRefractionButtonClicked(MyGUI::Widget* sender);
        void onWaterTextureSizeChanged(MyGUI::ComboBox* sender, size_t pos);
        void onWaterReflectionDetailChanged(MyGUI::ComboBox* sender, size_t pos);
        void onWaterRainRippleDetailChanged(MyGUI::ComboBox* sender, size_t pos);

        void onLightingMethodButtonChanged(MyGUI::ComboBox* sender, size_t pos);
        void onLightsResetButtonClicked(MyGUI::Widget* sender);
        void onJiggleOffsetResetButtonClicked(MyGUI::Widget* sender);
        void onVerletResetButtonClicked(MyGUI::Widget* sender);
        void onBakeBreastToNifButtonClicked(MyGUI::Widget* sender);
        void onJiggleAdvancedPanelToggleClicked(MyGUI::Widget* sender);
        void onJiggleAdvancedCloseClicked(MyGUI::Widget* sender);
        void onJiggleQuickAddAnyClicked(MyGUI::Widget* sender);
        void onJiggleQuickAddPlayerClicked(MyGUI::Widget* sender);
        void onJiggleQuickBlacklistClicked(MyGUI::Widget* sender);
        void onJiggleQuickClearClicked(MyGUI::Widget* sender);
        void onJiggleUseCurrentMeshClicked(MyGUI::Widget* sender);
        void onJiggleSaveMeshOffsetClicked(MyGUI::Widget* sender);
        void onJiggleRemoveMeshOffsetClicked(MyGUI::Widget* sender);
        void onJiggleAddBlacklistClicked(MyGUI::Widget* sender);
        void onJiggleRemoveBlacklistClicked(MyGUI::Widget* sender);
        void onJiggleEnableNpcClicked(MyGUI::Widget* sender);
        void onJiggleDisableNpcClicked(MyGUI::Widget* sender);
        void onJiggleClearNpcRuleClicked(MyGUI::Widget* sender);
        void refreshJiggleAdvancedPanel();
        void onMaxLightsChanged(MyGUI::ComboBox* sender, size_t pos);

        void onShadowResolutionChanged(MyGUI::ComboBox* sender, size_t pos);
        void onShadowUpdateIntervalChanged(MyGUI::ComboBox* sender, size_t pos);

        void onPrimaryLanguageChanged(MyGUI::ComboBox* sender, size_t pos) { onLanguageChanged(0, sender, pos); }
        void onSecondaryLanguageChanged(MyGUI::ComboBox* sender, size_t pos) { onLanguageChanged(1, sender, pos); }
        void onLanguageChanged(size_t langPriority, MyGUI::ComboBox* sender, size_t pos);
        void onGmstOverridesL10nChanged(MyGUI::Widget* sender);

        void onWindowModeChanged(MyGUI::ComboBox* sender, size_t pos);
        void onVSyncModeChanged(MyGUI::ComboBox* sender, size_t pos);

        void onRebindAction(MyGUI::Widget* sender);
        void onInputTabMouseWheel(MyGUI::Widget* sender, int rel);
        void onResetDefaultBindings(MyGUI::Widget* sender);
        void onResetDefaultBindingsAccept();
        void onKeyboardSwitchClicked(MyGUI::Widget* sender);
        void onControllerSwitchClicked(MyGUI::Widget* sender);

        void onWindowResize(MyGUI::Window* sender);

        void onScriptFilterChange(MyGUI::EditBox*);
        void onScriptListSelection(MyGUI::ListBox*, size_t index);

        void apply();

        void configureWidgets(MyGUI::Widget* widget, bool init);
        MyGUI::TextBox* getSliderLabel(MyGUI::ScrollBar* scroller) const;

        void layoutControlsBox();
        void renderScriptSettings();

        void computeMinimumWindowSize();

    private:
        void resetScrollbars();
        Files::ConfigurationManager& mCfgMgr;
    };
}

#endif
