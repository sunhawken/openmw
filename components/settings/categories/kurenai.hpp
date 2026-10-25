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
    };
}

#endif
