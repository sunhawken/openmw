#include "nifbonewriter.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/vfs/manager.hpp>

namespace
{
    constexpr std::array<std::string_view, 2> sBreastBones = { "bip01 l breast", "bip01 r breast" };

    bool isBreastBone(std::string_view name)
    {
        for (const std::string_view bone : sBreastBones)
            if (Misc::StringUtils::ciEqual(name, bone))
                return true;
        return false;
    }
}

namespace Misc::NifBoneWriter
{
    bool addBreastZ(const VFS::Manager& vfs, std::string_view meshFile, float deltaZ, std::string& error)
    {
        if (meshFile.empty())
        {
            error = "there is no currently worn body mesh";
            return false;
        }
        if (!std::isfinite(deltaZ))
        {
            error = "the requested Z adjustment is not finite";
            return false;
        }
        if (deltaZ == 0.f)
            return true;

        const VFS::Path::Normalized path(meshFile);
        const auto physicalPath = vfs.getPhysicalPath(path);
        if (!physicalPath)
        {
            error = "the selected mesh is inside an archive, not a writable loose NIF";
            return false;
        }

        std::error_code ec;
        const auto permissions = std::filesystem::status(*physicalPath, ec).permissions();
        if (ec || (permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none)
        {
            error = "the selected loose NIF is read-only";
            return false;
        }

        Nif::NIFFile nif(path);
        try
        {
            Nif::Reader reader(nif, nullptr);
            reader.parse(vfs.get(path));
        }
        catch (const std::exception& e)
        {
            error = e.what();
            return false;
        }

        std::array<std::size_t, sBreastBones.size()> zOffsets{};
        std::size_t found = 0;
        for (const auto& record : nif.mRecords)
        {
            const auto* object = dynamic_cast<const Nif::NiAVObject*>(record.get());
            if (!object || !isBreastBone(object->mName)
                || object->mTranslationOffset == std::numeric_limits<std::size_t>::max())
                continue;
            zOffsets[found++] = object->mTranslationOffset + 2 * sizeof(float);
        }
        if (found != zOffsets.size())
        {
            error = "the selected NIF does not contain both Bip01 L/R Breast nodes";
            return false;
        }

        std::fstream stream(*physicalPath, std::ios::in | std::ios::out | std::ios::binary);
        if (!stream)
        {
            error = "could not open the selected loose NIF for writing";
            return false;
        }
        for (const std::size_t zOffset : zOffsets)
        {
            float z = 0.f;
            stream.seekg(static_cast<std::streamoff>(zOffset));
            stream.read(reinterpret_cast<char*>(&z), sizeof(z));
            if (!stream || !std::isfinite(z))
            {
                error = "could not read a breast-node Z translation";
                return false;
            }
            z += deltaZ;
            stream.seekp(static_cast<std::streamoff>(zOffset));
            stream.write(reinterpret_cast<const char*>(&z), sizeof(z));
            if (!stream)
            {
                error = "could not write a breast-node Z translation";
                return false;
            }
        }
        return true;
    }
}
