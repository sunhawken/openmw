#ifndef OPENMW_ESM3_RELEASEDEQUIPMENTSTATE_H
#define OPENMW_ESM3_RELEASEDEQUIPMENTSTATE_H

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ESM
{
    class ESMReader;
    class ESMWriter;

    // Saved only on released world equipment. Posed arrays retain the drawable's
    // native local frame; mTransform maps it to the item's proper rigid frame.
    // Mesh topology, UVs and materials remain in VFS.
    // This preserves reflected shields and the actual equipped body-part model.
    struct ReleasedEquipmentState
    {
        struct Drawable
        {
            std::string mSourceName;
            uint32_t mSourceOrdinal = 0;
            uint32_t mSourceRecordIndex = std::numeric_limits<uint32_t>::max();
            uint32_t mSourceVertexCount = 0;
            uint64_t mSourceTopologyHash = 0;
            bool mClockwise = false;
            std::array<float, 16> mTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::vector<std::array<float, 3>> mVertices;
            std::vector<std::array<float, 3>> mNormals;
            std::vector<std::array<float, 4>> mTangents;
        };

        static constexpr std::size_t MaxDrawables = 128;
        static constexpr std::size_t MaxVertices = 262144;
        std::string mModel;
        float mMass = 1.f;
        std::vector<Drawable> mDrawables;

        bool valid() const;
        void load(ESMReader& esm);
        void save(ESMWriter& esm) const;
    };
}

#endif
