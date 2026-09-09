#include "filesystemarchive.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>

#include "pathutil.hpp"

#include <components/debug/debuglog.hpp>
#include <components/files/constrainedfilestream.hpp>
#include <components/files/conversion.hpp>

namespace VFS
{
    namespace
    {
        // On-disk format for the directory-walk cache. Bump the version whenever the layout changes.
        constexpr std::uint32_t sCacheMagic = 0x564d574f; // "OMWV"
        constexpr std::uint32_t sCacheVersion = 1;
    }

    FileSystemArchive::FileSystemArchive(const std::filesystem::path& path, const std::filesystem::path& cachePath)
        : mPath(path)
    {
        if (!cachePath.empty() && tryLoadCache(cachePath))
            return;

        build();

        if (!cachePath.empty())
            saveCache(cachePath);
    }

    void FileSystemArchive::build()
    {
        const auto str = mPath.u8string();
        std::size_t prefix = str.size();

        if (prefix > 0 && str[prefix - 1] != '\\' && str[prefix - 1] != '/')
            ++prefix;

        std::filesystem::recursive_directory_iterator iterator(
            mPath, std::filesystem::directory_options::follow_directory_symlink);

        for (auto it = std::filesystem::begin(iterator), end = std::filesystem::end(iterator); it != end;)
        {
            const std::filesystem::directory_entry& entry = *it;

            if (!entry.is_directory())
            {
                const std::filesystem::path& filePath = entry.path();
                const std::string proper = Files::pathToUnicodeString(filePath);
                VFS::Path::Normalized searchable(std::string_view{ proper }.substr(prefix));
                FileSystemArchiveFile file(filePath);

                const auto inserted = mIndex.emplace(std::move(searchable), std::move(file));
                if (!inserted.second)
                    Log(Debug::Warning)
                        << "Found duplicate file for '" << proper
                        << "', please check your file system for two files with the same name in different cases.";
            }

            // Exception thrown by the operator++ may not contain the context of the error like what exact path caused
            // the problem which makes it hard to understand what's going on when iteration happens over a directory
            // with thousands of files and subdirectories.
            const std::filesystem::path prevPath = entry.path();
            std::error_code ec;
            it.increment(ec);
            if (ec != std::error_code())
                throw std::runtime_error("Failed to recursively iterate over \"" + Files::pathToUnicodeString(mPath)
                    + "\" when incrementing to the next item from \"" + Files::pathToUnicodeString(prevPath)
                    + "\": " + ec.message());
        }
    }

    bool FileSystemArchive::tryLoadCache(const std::filesystem::path& cachePath)
    {
        std::error_code ec;
        if (!std::filesystem::exists(cachePath, ec))
            return false;

        const auto rootMtime = std::filesystem::last_write_time(mPath, ec);
        if (ec)
            return false;
        const std::int64_t rootMtimeCount = static_cast<std::int64_t>(rootMtime.time_since_epoch().count());

        std::ifstream in(cachePath, std::ios::binary);
        if (!in)
            return false;

        const auto readPod = [&](auto& value) { in.read(reinterpret_cast<char*>(&value), sizeof(value)); };
        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        readPod(magic);
        readPod(version);
        if (!in || magic != sCacheMagic || version != sCacheVersion)
            return false;

        std::int64_t storedMtime = 0;
        readPod(storedMtime);
        // The cache is only valid while the directory's own modification time is unchanged. Editing
        // the mod's files (add/remove/rename) bumps it and forces a fresh walk; toggling the setting
        // off/on or deleting the cache folder also forces a rebuild.
        if (!in || storedMtime != rootMtimeCount)
            return false;

        std::uint64_t count = 0;
        readPod(count);
        if (!in)
            return false;

        const auto readString = [&](std::string& out) -> bool {
            std::uint32_t len = 0;
            in.read(reinterpret_cast<char*>(&len), sizeof(len));
            if (!in)
                return false;
            out.resize(len);
            if (len != 0)
                in.read(out.data(), len);
            return static_cast<bool>(in);
        };

        std::map<VFS::Path::Normalized, FileSystemArchiveFile, std::less<>> index;
        std::string searchable;
        std::string absolute;
        for (std::uint64_t i = 0; i < count; ++i)
        {
            if (!readString(searchable) || !readString(absolute))
                return false;
            index.emplace(VFS::Path::Normalized(searchable), FileSystemArchiveFile(Files::pathFromUnicodeString(absolute)));
        }
        if (!in)
            return false;

        mIndex = std::move(index);
        Log(Debug::Info) << "Loaded VFS directory cache for " << Files::pathToUnicodeString(mPath) << " ("
                         << mIndex.size() << " files)";
        return true;
    }

    void FileSystemArchive::saveCache(const std::filesystem::path& cachePath) const
    {
        std::error_code ec;
        std::filesystem::create_directories(cachePath.parent_path(), ec);

        const auto rootMtime = std::filesystem::last_write_time(mPath, ec);
        if (ec)
        {
            Log(Debug::Warning) << "Not writing VFS directory cache for " << Files::pathToUnicodeString(mPath) << ": "
                                << ec.message();
            return;
        }
        const std::int64_t rootMtimeCount = static_cast<std::int64_t>(rootMtime.time_since_epoch().count());

        // Write to a temp file and rename over the target so a crash mid-write can't leave a corrupt cache.
        std::filesystem::path tmp = cachePath;
        tmp += ".tmp";

        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                Log(Debug::Warning) << "Failed to open VFS directory cache for writing: "
                                    << Files::pathToUnicodeString(tmp);
                return;
            }

            const auto writePod = [&](auto value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
            const auto writeString = [&](std::string_view value) {
                const std::uint32_t len = static_cast<std::uint32_t>(value.size());
                out.write(reinterpret_cast<const char*>(&len), sizeof(len));
                out.write(value.data(), static_cast<std::streamsize>(value.size()));
            };

            writePod(sCacheMagic);
            writePod(sCacheVersion);
            writePod(rootMtimeCount);
            writePod(static_cast<std::uint64_t>(mIndex.size()));
            for (const auto& [k, v] : mIndex)
            {
                writeString(k.value());
                writeString(Files::pathToUnicodeString(v.getPath()));
            }

            out.flush();
            if (!out)
            {
                out.close();
                std::filesystem::remove(tmp, ec);
                Log(Debug::Warning) << "Failed while writing VFS directory cache: " << Files::pathToUnicodeString(tmp);
                return;
            }
        }

        std::filesystem::rename(tmp, cachePath, ec);
        if (ec)
        {
            std::filesystem::remove(tmp, ec);
            Log(Debug::Warning) << "Failed to finalize VFS directory cache " << Files::pathToUnicodeString(cachePath)
                                << ": " << ec.message();
        }
    }

    void FileSystemArchive::listResources(FileMap& out)
    {
        for (auto& [k, v] : mIndex)
            out[k] = &v;
    }

    bool FileSystemArchive::contains(Path::NormalizedView file) const
    {
        return mIndex.find(file) != mIndex.end();
    }

    std::string FileSystemArchive::getDescription() const
    {
        return "DIR: " + Files::pathToUnicodeString(mPath);
    }

    // ----------------------------------------------------------------------------------

    FileSystemArchiveFile::FileSystemArchiveFile(const std::filesystem::path& path)
        : mPath(path)
    {
    }

    Files::IStreamPtr FileSystemArchiveFile::open()
    {
        return Files::openConstrainedFileStream(mPath);
    }

    std::filesystem::file_time_type FileSystemArchiveFile::getLastModified() const
    {
        return std::filesystem::last_write_time(mPath);
    }

    std::string FileSystemArchiveFile::getStem() const
    {
        return Files::pathToUnicodeString(mPath.stem());
    }

}
