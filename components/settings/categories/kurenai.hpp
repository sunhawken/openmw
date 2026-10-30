#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_KURENAI_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_KURENAI_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

#include <string>

namespace Settings
{
    // KURENAI: Red Ninja (PS2, in PCSX2) drives the player. Used only by builds made with KURENAI_BRIDGE_DIR.
    struct KurenaiCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<bool> mEnabled{ mIndex, "Kurenai", "enabled" };
        SettingValue<std::string> mBridgeName{ mIndex, "Kurenai", "bridge name" };
        SettingValue<float> mScale{ mIndex, "Kurenai", "scale", makeClampSanitizerFloat(0.01f, 100.0f) };
        SettingValue<std::string> mHeightMode{ mIndex, "Kurenai", "height mode",
            makeEnumSanitizerString({ "ground", "game" }) };
        SettingValue<std::string> mCameraMode{ mIndex, "Kurenai", "camera mode",
            makeEnumSanitizerString({ "openmw", "game" }) };
        SettingValue<bool> mAnswerCollision{ mIndex, "Kurenai", "answer collision queries" };
        SettingValue<bool> mCombat{ mIndex, "Kurenai", "combat" };
        SettingValue<float> mPeerTimeout{ mIndex, "Kurenai", "peer timeout", makeClampSanitizerFloat(0.1f, 60.0f) };
        SettingValue<bool> mInterpolate{ mIndex, "Kurenai", "interpolate" };
        SettingValue<bool> mPredict{ mIndex, "Kurenai", "predict" };
        SettingValue<std::string> mAnimationMap{ mIndex, "Kurenai", "animation map" };
        SettingValue<std::string> mStateLog{ mIndex, "Kurenai", "state log" };
        SettingValue<std::string> mPs2Animation{ mIndex, "Kurenai", "ps2 animation",
            makeEnumSanitizerString({ "off", "locomotion", "all" }) };
        SettingValue<std::string> mSkeletonMap{ mIndex, "Kurenai", "skeleton map" };
        SettingValue<bool> mShowStatus{ mIndex, "Kurenai", "show status" };
        SettingValue<bool> mGameHud{ mIndex, "Kurenai", "game hud" };
        SettingValue<bool> mHideMorrowindHud{ mIndex, "Kurenai", "hide morrowind hud" };
        SettingValue<std::string> mHdUi{ mIndex, "Kurenai", "hd ui" };
        SettingValue<float> mGameHudScale{ mIndex, "Kurenai", "game hud scale", makeClampSanitizerFloat(0.25f, 4.0f) };
        SettingValue<std::string> mStartMode{ mIndex, "Kurenai", "start mode",
            makeEnumSanitizerString({ "combat", "morrowind" }) };
        SettingValue<std::string> mModeToggleKey{ mIndex, "Kurenai", "mode toggle key" };
        SettingValue<std::string> mRecoverKey{ mIndex, "Kurenai", "recover key" };
        SettingValue<std::string> mControlsKey{ mIndex, "Kurenai", "controls key" };
        SettingValue<std::string> mSelectKey{ mIndex, "Kurenai", "select key" };
        SettingValue<float> mTestKillSeconds{ mIndex, "Kurenai", "test kill seconds", makeClampSanitizerFloat(0.0f, 3600.0f) };
        SettingValue<std::string> mJoystickMap{ mIndex, "Kurenai", "joystick map" };
        SettingValue<std::string> mMenuKey{ mIndex, "Kurenai", "menu key" };
        SettingValue<std::string> mWeaponKey{ mIndex, "Kurenai", "weapon key" };
        SettingValue<bool> mProxies{ mIndex, "Kurenai", "proxies" };
        SettingValue<float> mProxyRadius{ mIndex, "Kurenai", "proxy radius", makeClampSanitizerFloat(100.0f, 20000.0f) };
        SettingValue<bool> mLockOn{ mIndex, "Kurenai", "lock on" };
        SettingValue<float> mLockRange{ mIndex, "Kurenai", "lock range", makeClampSanitizerFloat(100.0f, 20000.0f) };
        SettingValue<int> mRadarRegion{ mIndex, "Kurenai", "radar region", makeClampSanitizerInt(0, 64) };
        SettingValue<float> mRadarRange{ mIndex, "Kurenai", "radar range", makeClampSanitizerFloat(100.0f, 50000.0f) };
        SettingValue<bool> mHookTargets{ mIndex, "Kurenai", "hook targets" };
        SettingValue<std::string> mSmokeModel{ mIndex, "Kurenai", "smoke model" };
        SettingValue<float> mSmokeRadius{ mIndex, "Kurenai", "smoke radius", makeClampSanitizerFloat(50.0f, 5000.0f) };
        SettingValue<bool> mGameLens{ mIndex, "Kurenai", "game lens" };
        SettingValue<std::string> mHealth{ mIndex, "Kurenai", "health",
            makeEnumSanitizerString({ "morrowind", "game", "off" }) };
        SettingValue<float> mMaxViewDistance{ mIndex, "Kurenai", "max view distance", makeMaxStrictSanitizerFloat(0) };
        SettingValue<std::string> mOutfit{ mIndex, "Kurenai", "outfit" };
        SettingValue<bool> mNativeBody{ mIndex, "Kurenai", "native body" };
        SettingValue<std::vector<std::string>> mNativeBodyModels{ mIndex, "Kurenai", "native body models" };
        SettingValue<std::string> mPs2Folder{ mIndex, "Kurenai", "ps2 folder" };
        SettingValue<std::string> mPs2Iso{ mIndex, "Kurenai", "ps2 iso" };
        SettingValue<std::string> mPs2Bios{ mIndex, "Kurenai", "ps2 bios" };
        SettingValue<std::string> mPs2Data{ mIndex, "Kurenai", "ps2 data" };
        SettingValue<std::string> mPs2Renderer{ mIndex, "Kurenai", "ps2 renderer",
            makeEnumSanitizerString({ "auto", "OpenGL", "Vulkan", "Software" }) };
        SettingValue<int> mPs2Volume{ mIndex, "Kurenai", "ps2 volume", makeClampSanitizerInt(0, 100) };
        SettingValue<bool> mPs2QuickStart{ mIndex, "Kurenai", "ps2 quick start" };
        SettingValue<bool> mPs2Window{ mIndex, "Kurenai", "ps2 window" };
    };
}

#endif
