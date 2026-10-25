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
        SettingValue<std::string> mAnimationMap{ mIndex, "Kurenai", "animation map" };
        SettingValue<std::string> mStateLog{ mIndex, "Kurenai", "state log" };
        SettingValue<std::string> mPs2Animation{ mIndex, "Kurenai", "ps2 animation",
            makeEnumSanitizerString({ "off", "locomotion", "all" }) };
        SettingValue<std::string> mSkeletonMap{ mIndex, "Kurenai", "skeleton map" };
        SettingValue<bool> mShowStatus{ mIndex, "Kurenai", "show status" };
        SettingValue<bool> mGameHud{ mIndex, "Kurenai", "game hud" };
        SettingValue<bool> mHideMorrowindHud{ mIndex, "Kurenai", "hide morrowind hud" };
        SettingValue<float> mGameHudScale{ mIndex, "Kurenai", "game hud scale", makeClampSanitizerFloat(0.25f, 4.0f) };
        SettingValue<std::string> mStartMode{ mIndex, "Kurenai", "start mode",
            makeEnumSanitizerString({ "combat", "morrowind" }) };
        SettingValue<std::string> mModeToggleKey{ mIndex, "Kurenai", "mode toggle key" };
        SettingValue<bool> mProxies{ mIndex, "Kurenai", "proxies" };
        SettingValue<float> mProxyRadius{ mIndex, "Kurenai", "proxy radius", makeClampSanitizerFloat(100.0f, 20000.0f) };
        SettingValue<bool> mHookTargets{ mIndex, "Kurenai", "hook targets" };
        SettingValue<std::string> mSmokeModel{ mIndex, "Kurenai", "smoke model" };
        SettingValue<float> mSmokeRadius{ mIndex, "Kurenai", "smoke radius", makeClampSanitizerFloat(50.0f, 5000.0f) };
    };
}

#endif
