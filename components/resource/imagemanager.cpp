#include "imagemanager.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <vector>

#include <osgDB/Registry>

#include <components/debug/debuglog.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "objectcache.hpp"

#ifdef OSG_LIBRARY_STATIC
// This list of plugins should match with the list in the top-level CMakelists.txt.
USE_OSGPLUGIN(png)
USE_OSGPLUGIN(tga)
USE_OSGPLUGIN(dds)
USE_OSGPLUGIN(jpeg)
USE_OSGPLUGIN(bmp)
USE_OSGPLUGIN(osg)
USE_SERIALIZER_WRAPPER_LIBRARY(osg)
#endif

namespace
{

    osg::ref_ptr<osg::Image> createWarningImage()
    {
        osg::ref_ptr<osg::Image> warningImage = new osg::Image;

        int width = 8, height = 8;
        warningImage->allocateImage(width, height, 1, GL_RGB, GL_UNSIGNED_BYTE);
        assert(warningImage->isDataContiguous());
        unsigned char* data = warningImage->data();
        for (int i = 0; i < width * height; ++i)
        {
            data[3 * i] = (255);
            data[3 * i + 1] = (0);
            data[3 * i + 2] = (255);
        }
        return warningImage;
    }

    bool isS3TC(osg::Image* image)
    {
        switch (image->getPixelFormat())
        {
            case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                return true;
        }
        return false;
    }

    bool checkSupported(osg::Image* image)
    {
        // not bothering with checks for other compression formats right now
        if (!isS3TC(image))
            return true;

        // hashtag yolo (CS might not have context when loading assets)
        if (!SceneUtil::glExtensionsReady())
            return true;

        return SceneUtil::getGLExtensions().isTextureCompressionS3TCSupported;
    }

    bool endsWith(std::string_view str, std::string_view suffix)
    {
        return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool startsWith(std::string_view str, std::string_view prefix)
    {
        return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
    }

    bool endsWithAny(std::string_view stem, std::initializer_list<std::string_view> suffixes)
    {
        for (std::string_view suffix : suffixes)
            if (endsWith(stem, suffix))
                return true;
        return false;
    }

    // Classify a texture by filename suffix so it can get a type-specific cap. VFS paths are
    // normalized to lower case. Order matters: more specific suffixes are checked first.
    enum class TextureType
    {
        Diffuse,
        Normal,
        Glow,
        Parallax,
        Material,
    };

    TextureType classifyTexture(std::string_view path)
    {
        const std::size_t dot = path.rfind('.');
        const std::string_view stem = dot == std::string_view::npos ? path : path.substr(0, dot);
        if (endsWithAny(stem, { "_n", "_nm", "_msn", "_normal" }))
            return TextureType::Normal;
        if (endsWithAny(stem, { "_g", "_glow", "_e", "_em" }))
            return TextureType::Glow;
        if (endsWithAny(stem, { "_p", "_h", "_parallax", "_height" }))
            return TextureType::Parallax;
        if (endsWithAny(stem, { "_rmaos", "_m", "_material", "_s", "_spec", "_specular" }))
            return TextureType::Material;
        return TextureType::Diffuse;
    }

    // The per-type cap, inheriting the general value when the type-specific one is 0.
    int typeCapFor(TextureType type)
    {
        const auto& g = Settings::general();
        const int general = g.mTextureDownscale;
        int specific = 0;
        switch (type)
        {
            case TextureType::Normal:
                specific = g.mTextureDownscaleNormalMaps;
                break;
            case TextureType::Glow:
                specific = g.mTextureDownscaleGlowMaps;
                break;
            case TextureType::Parallax:
                specific = g.mTextureDownscaleParallaxMaps;
                break;
            case TextureType::Material:
                specific = g.mTextureDownscaleMaterialMaps;
                break;
            case TextureType::Diffuse:
                break;
        }
        return specific > 0 ? specific : general;
    }

    // A per-folder override, if any, for @p path: the longest matching "prefix=cap" rule. Returns
    // true and sets @p outCap (which may be 0 = leave full size) when a folder rule applies.
    bool folderRuleCapFor(std::string_view path, int& outCap)
    {
        const auto& rules = Settings::general().mTextureDownscaleFolderRules.get();
        std::size_t bestLen = 0;
        bool found = false;
        for (const std::string& rule : rules)
        {
            const std::size_t eq = rule.rfind('=');
            if (eq == std::string::npos)
                continue;
            std::string prefix = Misc::StringUtils::lowerCase(rule.substr(0, eq));
            for (char& c : prefix)
                if (c == '\\')
                    c = '/';
            std::size_t begin = prefix.find_first_not_of(" \t");
            std::size_t end = prefix.find_last_not_of(" \t");
            if (begin == std::string::npos)
                continue;
            const std::string_view trimmed(prefix.data() + begin, end - begin + 1);
            if (trimmed.size() > bestLen && startsWith(path, trimmed))
            {
                bestLen = trimmed.size();
                outCap = std::atoi(rule.c_str() + eq + 1);
                found = true;
            }
        }
        return found;
    }

    // The largest-dimension cap (in pixels) to apply to this texture, or 0 for "leave full size".
    // Per-folder rules take precedence over per-type caps.
    int downscaleCapFor(std::string_view path)
    {
        int folderCap = 0;
        if (folderRuleCapFor(path, folderCap))
            return folderCap;
        return typeCapFor(classifyTexture(path));
    }

    // Number of top mip levels to drop so the base dimension is <= cap, keeping at least one level.
    unsigned int mipsToSkip(int s, int t, unsigned int levels, int cap)
    {
        if (cap <= 0 || levels <= 1)
            return 0;
        unsigned int skip = 0;
        int w = s;
        int h = t;
        while ((w > cap || h > cap) && skip + 1 < levels)
        {
            w = std::max(1, w >> 1);
            h = std::max(1, h >> 1);
            ++skip;
        }
        return skip;
    }

    // Check that this is the simple image layout used by DDS and the usual OpenMW texture readers:
    // a tightly-packed, complete 2D mip chain. OSG's mip offsets are metadata supplied by each image
    // reader; they are not an allocation-size API. Do not reinterpret arbitrary offsets as a contiguous
    // byte range, since that can make a malformed or non-standard image read past its backing storage.
    bool getCanonicalMipLayout(const osg::Image& src, std::vector<std::size_t>& offsets, std::size_t& total)
    {
        const unsigned int levels = src.getNumMipmapLevels();
        if (!src.valid() || src.r() != 1 || !src.isDataContiguous() || levels < 2)
            return false;

        offsets.clear();
        offsets.reserve(levels);
        total = 0;
        for (unsigned int level = 0; level < levels; ++level)
        {
            const std::size_t offset = level == 0 ? 0 : src.getMipmapLevels()[level - 1];
            if (offset != total)
                return false;

            const int width = std::max(1, src.s() >> level);
            const int height = std::max(1, src.t() >> level);
            const std::size_t size = osg::Image::computeImageSizeInBytes(
                width, height, 1, src.getPixelFormat(), src.getDataType(), src.getPacking());
            if (size == 0 || size > std::numeric_limits<std::size_t>::max() - total)
                return false;

            offsets.push_back(offset);
            total += size;
        }

        // This also rejects unusual row/slice padding and reader-specific image layouts. It is safer
        // to leave those textures untouched than to risk making an invalid replacement image.
        return total <= std::numeric_limits<unsigned int>::max()
            && total == src.getTotalSizeInBytesIncludingMipmaps();
    }

    // Build a copy of @p src that drops @p skip top mip levels: its base level becomes old level
    // @p skip, with the remaining smaller mips kept. This is a plain byte copy of a validated mip stack
    // from that level down, so it remains valid for compressed (DXT/S3TC) images too - no resampling.
    osg::ref_ptr<osg::Image> dropTopMips(const osg::Image& src, unsigned int skip)
    {
        const unsigned int levels = src.getNumMipmapLevels();
        std::vector<std::size_t> offsets;
        std::size_t total = 0;
        if (skip == 0 || skip >= levels || !getCanonicalMipLayout(src, offsets, total))
            return nullptr;

        const unsigned char* data0 = src.data();
        const std::size_t base = offsets[skip];
        const std::size_t newSize = total - base;

        unsigned char* newData = new unsigned char[newSize];
        std::memcpy(newData, data0 + base, newSize);

        const int ns = std::max(1, src.s() >> skip);
        const int nt = std::max(1, src.t() >> skip);
        const int nr = std::max(1, src.r() >> skip);

        osg::ref_ptr<osg::Image> dst = new osg::Image;
        dst->setFileName(src.getFileName());
        dst->setOrigin(src.getOrigin());
        dst->setImage(ns, nt, nr, src.getInternalTextureFormat(), src.getPixelFormat(), src.getDataType(), newData,
            osg::Image::USE_NEW_DELETE, src.getPacking());

        osg::Image::MipmapDataType mipmaps;
        for (unsigned int level = skip + 1; level < levels; ++level)
            mipmaps.push_back(static_cast<unsigned int>(offsets[level] - base));
        dst->setMipmapLevels(mipmaps);

        return dst;
    }

    // Cap a mipmapped texture's resolution at load time by handing the game a lower mip as the base
    // level. Ported concept from TextureDownscaler: saves VRAM without resampling; textures without
    // mipmaps are left untouched.
    osg::ref_ptr<osg::Image> applyTextureDownscale(osg::ref_ptr<osg::Image> image, std::string_view path)
    {
        const int cap = downscaleCapFor(path);
        if (cap <= 0)
            return image;

        const unsigned int levels = image->getNumMipmapLevels();
        const unsigned int skip = mipsToSkip(image->s(), image->t(), levels, cap);
        if (skip == 0)
            return image;

        osg::ref_ptr<osg::Image> scaled = dropTopMips(*image, skip);
        if (!scaled)
        {
            if (Settings::general().mTextureDownscaleDebug)
                Log(Debug::Verbose) << "Skipped downscaling non-canonical mip chain: " << path;
            return image;
        }
        if (Settings::general().mTextureDownscaleDebug)
            Log(Debug::Verbose) << "Downscaled texture " << path << " from " << image->s() << "x" << image->t()
                                << " to " << scaled->s() << "x" << scaled->t() << " (skipped " << skip << " mip level(s))";
        return scaled;
    }

}

namespace Resource
{

    ImageManager::ImageManager(const VFS::Manager* vfs, double expiryDelay)
        : ResourceManager(vfs, expiryDelay)
        , mWarningImage(createWarningImage())
        , mOptions(new osgDB::Options("dds_dxt1_detect_rgba ignoreTga2Fields"))
    {
    }

    ImageManager::~ImageManager() {}

    osg::ref_ptr<osg::Image> ImageManager::getImage(VFS::Path::NormalizedView path, bool disableFlip)
    {
        osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(path);
        if (obj)
            return osg::ref_ptr<osg::Image>(static_cast<osg::Image*>(obj.get()));
        else
        {
            Files::IStreamPtr stream;
            try
            {
                stream = mVFS->get(path);
            }
            catch (std::exception& e)
            {
                Log(Debug::Error) << "Failed to open image: " << e.what();
                mCache->addEntryToObjectCache(path.value(), mWarningImage);
                return mWarningImage;
            }

            const std::string ext(Misc::getFileExtension(path.value()));
            osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension(ext);
            if (!reader)
            {
                Log(Debug::Error) << "Error loading " << path << ": no readerwriter for '" << ext << "' found";
                mCache->addEntryToObjectCache(path.value(), mWarningImage);
                return mWarningImage;
            }

            bool killAlpha = false;
            if (reader->supportedExtensions().count("tga"))
            {
                // Morrowind ignores the alpha channel of 16bpp TGA files even when the header says not to
                unsigned char header[18];
                stream->read((char*)header, 18);
                if (stream->gcount() != 18)
                {
                    Log(Debug::Error) << "Error loading " << path << ": couldn't read TGA header";
                    mCache->addEntryToObjectCache(path.value(), mWarningImage);
                    return mWarningImage;
                }
                int type = header[2];
                int depth;
                if (type == 1 || type == 9)
                    depth = header[7];
                else
                    depth = header[16];
                int alphaBPP = header[17] & 0x0F;
                killAlpha = depth == 16 && alphaBPP == 1;
                stream->seekg(0);
            }

            osgDB::ReaderWriter::ReadResult result = reader->readImage(*stream, mOptions);
            if (!result.success())
            {
                Log(Debug::Error) << "Error loading " << path << ": " << result.message() << " code "
                                  << result.status();
                mCache->addEntryToObjectCache(path.value(), mWarningImage);
                return mWarningImage;
            }

            osg::ref_ptr<osg::Image> image = result.getImage();

            image->setFileName(std::string(path.value()));
            if (!checkSupported(image))
            {
                static bool uncompress = (getenv("OPENMW_DECOMPRESS_TEXTURES") != nullptr);
                if (!uncompress)
                {
                    Log(Debug::Error) << "Error loading " << path << ": no S3TC texture compression support installed";
                    mCache->addEntryToObjectCache(path.value(), mWarningImage);
                    return mWarningImage;
                }
                else
                {
                    // decompress texture in software if not supported by GPU
                    // requires update to getColor() to be released with OSG 3.6
                    osg::ref_ptr<osg::Image> newImage = new osg::Image;
                    newImage->setFileName(image->getFileName());
                    newImage->setOrigin(image->getOrigin());
                    newImage->allocateImage(image->s(), image->t(), image->r(),
                        image->isImageTranslucent() ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE);
                    for (int s = 0; s < image->s(); ++s)
                        for (int t = 0; t < image->t(); ++t)
                            for (int r = 0; r < image->r(); ++r)
                                newImage->setColor(image->getColor(s, t, r), s, t, r);
                    image = newImage;
                }
            }
            else if (killAlpha)
            {
                osg::ref_ptr<osg::Image> newImage = new osg::Image;
                newImage->setFileName(image->getFileName());
                newImage->setOrigin(image->getOrigin());
                newImage->allocateImage(image->s(), image->t(), image->r(), GL_RGB, GL_UNSIGNED_BYTE);
                // OSG just won't write the alpha as there's nowhere to put it.
                for (int s = 0; s < image->s(); ++s)
                    for (int t = 0; t < image->t(); ++t)
                        for (int r = 0; r < image->r(); ++r)
                            newImage->setColor(image->getColor(s, t, r), s, t, r);
                image = newImage;
            }

            // OSG might not set the right origin for DDS
            if (ext == "dds")
                image->setOrigin(osg::Image::TOP_LEFT);

            // Convert the image to the convention we expect
            if (image->getOrigin() == osg::Image::BOTTOM_LEFT && !disableFlip)
            {
                if (image->isCompressed() && !isS3TC(image))
                {
                    // This is most likely a KTX texture that OSG can't flip
                    // We don't want it to be corrupted or displayed incorrectly, so bail
                    // OSGoS *can* flip RGTC, but we can't verify that (yet?)
                    Log(Debug::Error) << "Error loading " << path << ": cannot flip non-S3TC compressed texture";
                    mCache->addEntryToObjectCache(path.value(), mWarningImage);
                    return mWarningImage;
                }

                image->flipVertical();
                image->setOrigin(osg::Image::TOP_LEFT);
            }

            image = applyTextureDownscale(image, path.value());

            mCache->addEntryToObjectCache(path.value(), image);
            return image;
        }
    }

    osg::Image* ImageManager::getWarningImage()
    {
        return mWarningImage;
    }

    void ImageManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Image", frameNumber, mCache->getStats(), *stats);
    }

}
