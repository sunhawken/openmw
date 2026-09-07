#include "imagemanager.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string_view>

#include <osgDB/Registry>

#include <components/debug/debuglog.hpp>
#include <components/misc/pathhelpers.hpp>
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

    // Normal maps get their own size cap. VFS paths are normalized to lower case.
    bool isNormalMap(std::string_view path)
    {
        const std::size_t dot = path.rfind('.');
        const std::string_view stem = dot == std::string_view::npos ? path : path.substr(0, dot);
        return endsWith(stem, "_n") || endsWith(stem, "_nm") || endsWith(stem, "_msn") || endsWith(stem, "_normal");
    }

    // The largest-dimension cap (in pixels) to apply to this texture, or 0 for "leave full size".
    int downscaleCapFor(std::string_view path)
    {
        const int general = Settings::general().mTextureDownscale;
        if (isNormalMap(path))
        {
            const int normal = Settings::general().mTextureDownscaleNormalMaps;
            return normal > 0 ? normal : general;
        }
        return general;
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

    // Build a copy of @p src that drops @p skip top mip levels: its base level becomes old level
    // @p skip, with the remaining smaller mips kept. This is a plain byte copy of the mip stack from
    // that level down, so it is valid for compressed (DXT/S3TC) images too - no resampling.
    osg::ref_ptr<osg::Image> dropTopMips(const osg::Image& src, unsigned int skip)
    {
        const unsigned int levels = src.getNumMipmapLevels();
        const unsigned char* data0 = src.getMipmapData(0);
        const auto levelOffset = [&](unsigned int level) -> std::size_t {
            return static_cast<std::size_t>(src.getMipmapData(level) - data0);
        };

        const std::size_t base = levelOffset(skip);
        const std::size_t total = src.getTotalSizeInBytesIncludingMipmaps();
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
            mipmaps.push_back(static_cast<unsigned int>(levelOffset(level) - base));
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
