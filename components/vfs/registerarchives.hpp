#ifndef OPENMW_COMPONENTS_VFS_REGISTER_ARCHIVES_H
#define OPENMW_COMPONENTS_VFS_REGISTER_ARCHIVES_H

#include <filesystem>

#include <components/files/collections.hpp>

namespace ToUTF8
{
    class StatelessUtf8Encoder;
}

namespace VFS
{
    class Manager;

    /// @brief Register BSA and file system archives based on the given OpenMW configuration.
    /// @param cacheDir if non-empty, loose-file data directories cache their recursive directory walk
    /// under this directory (one cache file per data dir) to speed up subsequent startups.
    void registerArchives(VFS::Manager* vfs, const Files::Collections& collections,
        const std::vector<std::string>& archives, bool useLooseFiles, const ToUTF8::StatelessUtf8Encoder* encoder,
        const std::filesystem::path& cacheDir = {});
}

#endif
