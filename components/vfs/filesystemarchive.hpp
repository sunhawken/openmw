#ifndef OPENMW_COMPONENTS_RESOURCE_FILESYSTEMARCHIVE_H
#define OPENMW_COMPONENTS_RESOURCE_FILESYSTEMARCHIVE_H

#include "archive.hpp"
#include "file.hpp"

#include <filesystem>
#include <string>

namespace VFS
{

    class FileSystemArchiveFile : public File
    {
    public:
        FileSystemArchiveFile(const std::filesystem::path& path);

        Files::IStreamPtr open() override;

        std::filesystem::file_time_type getLastModified() const override;

        std::string getStem() const override;

        const std::filesystem::path& getPath() const { return mPath; }

    private:
        std::filesystem::path mPath;
    };

    class FileSystemArchive : public Archive
    {
    public:
        /// @param cachePath if non-empty, the file index for @p path is loaded from this cache file
        /// (skipping the recursive directory walk) when it exists and is still valid, and (re)written
        /// to it after a real walk. Ported concept from CRDW: cache the recursive directory walk to
        /// speed up startup, which is especially slow over a virtual filesystem overlay (e.g. MO2).
        explicit FileSystemArchive(const std::filesystem::path& path, const std::filesystem::path& cachePath = {});

        void listResources(FileMap& out) override;

        bool contains(Path::NormalizedView file) const override;

        std::string getDescription() const override;

    private:
        void build();
        bool tryLoadCache(const std::filesystem::path& cachePath);
        void saveCache(const std::filesystem::path& cachePath) const;

        std::map<VFS::Path::Normalized, FileSystemArchiveFile, std::less<>> mIndex;
        std::filesystem::path mPath;
    };

}

#endif
