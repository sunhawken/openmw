/*
  OpenMW - The completely unofficial reimplementation of Morrowind
  Copyright (C) 2008-2010  Nicolay Korslund
  Email: < korslund@gmail.com >
  WWW: https://openmw.org/

  This file (bsafile.hpp) is part of the OpenMW package.

  OpenMW is distributed as free software: you can redistribute it
  and/or modify it under the terms of the GNU General Public License
  version 3, as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  version 3 along with this program. If not, see
  https://www.gnu.org/licenses/ .

 */

#ifndef OPENMW_COMPONENTS_BSA_BSAFILE_HPP
#define OPENMW_COMPONENTS_BSA_BSAFILE_HPP

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include <components/files/conversion.hpp>
#include <components/files/istreamptr.hpp>

namespace boost::iostreams
{
    class mapped_file_source;
}

namespace Bsa
{

    enum class BsaVersion : std::uint32_t
    {
        Unknown = 0x0,
        Uncompressed = 0x100,
        Compressed = 0x415342, // B, S, A,
        BA2GNRL, // used by FO4, BSA which contains files
        BA2DX10 // used by FO4, BSA which contains textures
    };

    /**
       This class is used to read "Bethesda Archive Files", or BSAs.
     */
    class BSAFile
    {
    public:
#pragma pack(push)
#pragma pack(1)
        struct Hash
        {
            uint32_t mLow;
            uint32_t mHigh;
        };
#pragma pack(pop)

        /// Represents one file entry in the archive
        struct FileStruct
        {
            // File size and offset in file. We store the offset from the
            // beginning of the file, not the offset into the data buffer
            // (which is what is stored in the archive.)
            uint32_t mFileSize = 0;
            uint32_t mOffset = 0;
            Hash mHash{};
            uint32_t mNameOffset = 0;
            uint32_t mNameSize = 0;
            std::vector<char>* mNamesBuffer = nullptr;

            std::string_view name() const { return std::string_view(mNamesBuffer->data() + mNameOffset, mNameSize); }
        };
        typedef std::vector<FileStruct> FileList;

    protected:
        bool mHasChanged = false;

        /// True when an archive has been loaded
        bool mIsLoaded = false;

        /// Table of files in this archive
        FileList mFiles;

        /// Filename string buffer
        std::vector<char> mStringBuf;

        /// Used for error messages
        std::filesystem::path mFilepath;

        /// Optional read-only memory map of the whole archive. When present, getFile() serves
        /// entries directly from the demand-paged mapping instead of a buffered file stream,
        /// cutting per-asset read latency (ported concept from Faster-File-Copy's uncompressed
        /// path). Set up in open() when memory mapping is enabled; null means fall back to file IO.
        std::shared_ptr<boost::iostreams::mapped_file_source> mMemoryMap;

        /// Process-wide switch (set once at startup from the "bsa memory mapping" setting) that
        /// controls whether open() memory-maps archives. Off for standalone tools by default.
        static bool sUseMemoryMapping;

        /// Error handling
        [[noreturn]] void fail(const std::string& msg) const;

        /// Read header information from the input source
        virtual void readHeader(std::istream& input);
        virtual void writeHeader();

    public:
        /* -----------------------------------
         * BSA management methods
         * -----------------------------------
         */

        virtual ~BSAFile()
        {
            close();
        }

        /// Open an archive file.
        void open(const std::filesystem::path& file);

        void close();

        /// Enable/disable memory mapping of archives for all subsequently opened BSAFiles.
        static void setUseMemoryMapping(bool enabled) { sUseMemoryMapping = enabled; }

        /* -----------------------------------
         * Archive file routines
         * -----------------------------------
         */

        /** Open a file contained in the archive.
         * @note Thread safe.
         */
        Files::IStreamPtr getFile(const FileStruct* file);

        void addFile(const std::string& filename, std::istream& file);

        /// Get a list of all files
        /// @note Thread safe.
        const FileList& getList() const
        {
            return mFiles;
        }

        std::string getFilename() const
        {
            return Files::pathToUnicodeString(mFilepath);
        }

        const std::filesystem::path& getPath() const
        {
            return mFilepath;
        }

        // checks version of BSA from file header
        static BsaVersion detectVersion(const std::filesystem::path& filePath);
    };
}

#endif
