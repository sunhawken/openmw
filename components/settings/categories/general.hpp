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
        // Per-type caps identified by filename suffix. 0 = use the general "texture downscale" value.
        // Normal maps: _n, _nm, _msn, _normal.
        SettingValue<int> mTextureDownscaleNormalMaps{ mIndex, "General", "texture downscale normal maps",
            makeClampSanitizerInt(0, 16384) };
        // Glow/emissive maps: _g, _glow, _e, _em.
        SettingValue<int> mTextureDownscaleGlowMaps{ mIndex, "General", "texture downscale glow maps",
            makeClampSanitizerInt(0, 16384) };
        // Parallax/height maps: _p, _h, _parallax, _height.
        SettingValue<int> mTextureDownscaleParallaxMaps{ mIndex, "General", "texture downscale parallax maps",
            makeClampSanitizerInt(0, 16384) };
        // Material/specular maps: _rmaos, _m, _material, _s, _spec, _specular.
        SettingValue<int> mTextureDownscaleMaterialMaps{ mIndex, "General", "texture downscale material maps",
            makeClampSanitizerInt(0, 16384) };
        // Per-folder overrides that take precedence over the type caps: a comma-separated list of
        // "path prefix=maxpx" rules (e.g. "textures/tr/=2048, textures/ui/=0"). The longest matching
        // prefix wins; a rule value of 0 leaves that folder's textures at full size.
        SettingValue<std::vector<std::string>> mTextureDownscaleFolderRules{ mIndex, "General",
            "texture downscale folder rules" };
        // Log each texture that gets downscaled (throttled to Debug::Verbose).
        SettingValue<bool> mTextureDownscaleDebug{ mIndex, "General", "texture downscale debug" };
    };
}

#endif
