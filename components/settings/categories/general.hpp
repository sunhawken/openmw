#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_GENERAL_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_GENERAL_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

#include <osg/Math>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <cstdint>
#include <string>
#include <string_view>

namespace Settings
{
    struct GeneralCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<int> mAnisotropy{ mIndex, "General", "anisotropy", makeClampSanitizerInt(0, 16) };
        SettingValue<std::string> mScreenshotFormat{ mIndex, "General", "screenshot format",
            makeEnumSanitizerString({ "jpg", "png", "tga" }) };
        SettingValue<std::string> mTextureMagFilter{ mIndex, "General", "texture mag filter",
            makeEnumSanitizerString({ "nearest", "linear" }) };
        SettingValue<std::string> mTextureMinFilter{ mIndex, "General", "texture min filter",
            makeEnumSanitizerString({ "nearest", "linear" }) };
        SettingValue<std::string> mTextureMipmap{ mIndex, "General", "texture mipmap",
            makeEnumSanitizerString({ "none", "nearest", "linear" }) };
        SettingValue<bool> mNotifyOnSavedScreenshot{ mIndex, "General", "notify on saved screenshot" };
        SettingValue<std::vector<std::string>> mPreferredLocales{ mIndex, "General", "preferred locales" };
        SettingValue<bool> mGmstOverridesL10n{ mIndex, "General", "gmst overrides l10n" };
        SettingValue<std::size_t> mLogBufferSize{ mIndex, "General", "log buffer size" };
        SettingValue<std::size_t> mConsoleHistoryBufferSize{ mIndex, "General", "console history buffer size" };
        // Cache each loose-file data directory's recursive directory walk to speed up startup
        // (ported concept from CRDW), especially over a virtual filesystem overlay like MO2. The
        // cache auto-rebuilds when a data directory's modification time changes.
        SettingValue<bool> mVfsDirectoryCache{ mIndex, "General", "vfs directory cache" };
        // Cap the resolution of mipmapped textures at load time by skipping the top mip level(s)
        // (no resampling), to save VRAM (ported concept from TextureDownscaler). Max width/height
        // in pixels; 0 disables.
        SettingValue<int> mTextureDownscale{ mIndex, "General", "texture downscale",
            makeClampSanitizerInt(0, 16384) };
        // Separate cap for normal maps (filenames ending _n, _nm, _msn, _normal). 0 = use the
        // general "texture downscale" value.
        SettingValue<int> mTextureDownscaleNormalMaps{ mIndex, "General", "texture downscale normal maps",
            makeClampSanitizerInt(0, 16384) };
        // Log each texture that gets downscaled (throttled to Debug::Verbose).
        SettingValue<bool> mTextureDownscaleDebug{ mIndex, "General", "texture downscale debug" };
    };
}

#endif
