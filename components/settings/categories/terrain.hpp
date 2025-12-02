#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_TERRAIN_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_TERRAIN_H

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
    struct TerrainCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<bool> mDistantTerrain{ mIndex, "Terrain", "distant terrain" };
        SettingValue<float> mLodFactor{ mIndex, "Terrain", "lod factor", makeMaxStrictSanitizerFloat(0) };
        SettingValue<int> mVertexLodMod{ mIndex, "Terrain", "vertex lod mod" };
        SettingValue<int> mCompositeMapLevel{ mIndex, "Terrain", "composite map level", makeMaxSanitizerInt(-3) };
        SettingValue<int> mCompositeMapResolution{ mIndex, "Terrain", "composite map resolution",
            makeMaxSanitizerInt(1) };
        SettingValue<float> mMaxCompositeGeometrySize{ mIndex, "Terrain", "max composite geometry size",
            makeMaxSanitizerFloat(1) };
        SettingValue<bool> mDebugChunks{ mIndex, "Terrain", "debug chunks" };
        SettingValue<bool> mObjectPaging{ mIndex, "Terrain", "object paging" };
        SettingValue<bool> mObjectPagingActiveGrid{ mIndex, "Terrain", "object paging active grid" };
        SettingValue<float> mObjectPagingMergeFactor{ mIndex, "Terrain", "object paging merge factor",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<float> mObjectPagingMinSize{ mIndex, "Terrain", "object paging min size",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<float> mObjectPagingMinSizeMergeFactor{ mIndex, "Terrain", "object paging min size merge factor",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<float> mObjectPagingMinSizeCostMultiplier{ mIndex, "Terrain",
            "object paging min size cost multiplier", makeMaxStrictSanitizerFloat(0) };
        SettingValue<bool> mWaterCulling{ mIndex, "Terrain", "water culling" };

        // Snow deformation settings
        SettingValue<bool> mSnowDeformationEnabled{ mIndex, "Terrain", "snow deformation enabled" };
        SettingValue<int> mSnowMaxFootprints{ mIndex, "Terrain", "snow max footprints",
            makeClampSanitizerInt(1, 500) };
        SettingValue<float> mSnowFootprintRadius{ mIndex, "Terrain", "snow footprint radius",
            makeMaxStrictSanitizerFloat(1.0f) };
        SettingValue<float> mSnowDeformationDepth{ mIndex, "Terrain", "snow deformation depth",
            makeMaxStrictSanitizerFloat(1.0f) };
        SettingValue<float> mSnowDecayTime{ mIndex, "Terrain", "snow decay time",
            makeMaxStrictSanitizerFloat(1.0f) };
        // Camera depth = how much of the body is captured (smaller = only feet, larger = full body)
        SettingValue<float> mSnowCameraDepth{ mIndex, "Terrain", "snow camera depth",
            makeMaxStrictSanitizerFloat(1.0f) };
        // Blur spread = smoothness of deformation edges (higher = smoother/wider blur)
        SettingValue<float> mSnowBlurSpread{ mIndex, "Terrain", "snow blur spread",
            makeMaxStrictSanitizerFloat(0.1f) };

        // Ash deformation settings
        SettingValue<bool> mAshDeformationEnabled{ mIndex, "Terrain", "ash deformation enabled" };
        SettingValue<float> mAshFootprintRadius{ mIndex, "Terrain", "ash footprint radius",
            makeMaxStrictSanitizerFloat(1.0f) };
        SettingValue<float> mAshDeformationDepth{ mIndex, "Terrain", "ash deformation depth",
            makeMaxStrictSanitizerFloat(1.0f) };
        SettingValue<float> mAshDecayTime{ mIndex, "Terrain", "ash decay time",
            makeMaxStrictSanitizerFloat(1.0f) };
        SettingValue<float> mAshCameraDepth{ mIndex, "Terrain", "ash camera depth",
            makeMaxStrictSanitizerFloat(1.0f) };
        SettingValue<float> mAshBlurSpread{ mIndex, "Terrain", "ash blur spread",
            makeMaxStrictSanitizerFloat(0.1f) };

        // Mud deformation settings
        SettingValue<bool> mMudDeformationEnabled{ mIndex, "Terrain", "mud deformation enabled" };
        SettingValue<float> mMudFootprintRadius{ mIndex, "Terrain", "mud footprint radius",
            makeMaxStrictSanitizerFloat(1.0f) };
        SettingValue<float> mMudDeformationDepth{ mIndex, "Terrain", "mud deformation depth",
            makeMaxStrictSanitizerFloat(1.0f) };
        SettingValue<float> mMudDecayTime{ mIndex, "Terrain", "mud decay time",
            makeMaxStrictSanitizerFloat(1.0f) };
        SettingValue<float> mMudCameraDepth{ mIndex, "Terrain", "mud camera depth",
            makeMaxStrictSanitizerFloat(1.0f) };
        SettingValue<float> mMudBlurSpread{ mIndex, "Terrain", "mud blur spread",
            makeMaxStrictSanitizerFloat(0.1f) };
    };
}

#endif
