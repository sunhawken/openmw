#include "chunkmanager.hpp"

#include <osg/Material>
#include <osg/Texture2D>

#include <osgUtil/IncrementalCompileOperation>

#include <components/debug/debuglog.hpp>
#include <components/esm/util.hpp>
#include <components/resource/objectcache.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/sceneutil/lightmanager.hpp>

#include "compositemaprenderer.hpp"
#include "material.hpp"
#include "storage.hpp"
#include "terraindrawable.hpp"
#include "terrainsubdivider.hpp"
#include "texturemanager.hpp"

namespace Terrain
{

    struct UpdateTextureFilteringFunctor
    {
        UpdateTextureFilteringFunctor(Resource::SceneManager* sceneMgr)
            : mSceneManager(sceneMgr)
        {
        }
        Resource::SceneManager* mSceneManager;

        void operator()(ChunkKey, osg::Object* obj)
        {
            TerrainDrawable* drawable = static_cast<TerrainDrawable*>(obj);
            CompositeMap* composite = drawable->getCompositeMap();
            if (composite && composite->mTexture)
                mSceneManager->applyFilterSettings(composite->mTexture);
        }
    };

    ChunkManager::ChunkManager(Storage* storage, Resource::SceneManager* sceneMgr, TextureManager* textureManager,
        CompositeMapRenderer* renderer, ESM::RefId worldspace, double expiryDelay)
        : GenericResourceManager<ChunkKey>(nullptr, expiryDelay)
        , QuadTreeWorld::ChunkManager(worldspace)
        , mStorage(storage)
        , mSceneManager(sceneMgr)
        , mTextureManager(textureManager)
        , mCompositeMapRenderer(renderer)
        , mNodeMask(0)
        , mCompositeMapSize(512)
        , mCompositeMapLevel(1.f)
        , mMaxCompGeometrySize(1.f)
        , mPlayerPosition(0.f, 0.f, 0.f)
        , mLastCacheClearPosition(0.f, 0.f, 0.f)
        , mSubdivisionTracker(std::make_unique<SubdivisionTracker>())
    {
        mMultiPassRoot = new osg::StateSet;
        mMultiPassRoot->setRenderingHint(osg::StateSet::OPAQUE_BIN);
        osg::ref_ptr<osg::Material> material(new osg::Material);
        material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
        mMultiPassRoot->setAttributeAndModes(material, osg::StateAttribute::ON);
    }

    osg::ref_ptr<osg::Node> ChunkManager::getChunk(float size, const osg::Vec2f& center, unsigned char lod,
        unsigned int lodFlags, bool activeGrid, const osg::Vec3f& viewPoint, bool compile)
    {
        // Override lod with the vertexLodMod adjusted value.
        // TODO: maybe we can refactor this code by moving all vertexLodMod code into this class.
        lod = static_cast<unsigned char>(lodFlags >> (4 * 4));

        const ChunkKey key{ .mCenter = center, .mLod = lod, .mLodFlags = lodFlags };
        if (osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(key))
        {
            return static_cast<osg::Node*>(obj.get());
        }

        const TerrainDrawable* templateGeometry = nullptr;
        const TemplateKey templateKey{ .mCenter = center, .mLod = lod };
        const auto pair = mCache->lowerBound(templateKey);
        if (pair.has_value() && templateKey == TemplateKey{ .mCenter = pair->first.mCenter, .mLod = pair->first.mLod })
            templateGeometry = static_cast<const TerrainDrawable*>(pair->second.get());

        osg::ref_ptr<osg::Node> node = createChunk(size, center, lod, lodFlags, compile, templateGeometry, viewPoint);
        mCache->addEntryToObjectCache(key, node.get());
        return node;
    }

    void ChunkManager::updateTextureFiltering()
    {
        UpdateTextureFilteringFunctor f(mSceneManager);
        mCache->call(f);
    }

    void ChunkManager::setPlayerPosition(const osg::Vec3f& pos)
    {
        // Calculate how far player has moved since last cache clear (horizontal distance only)
        osg::Vec2f currentPos2D(pos.x(), pos.y());
        osg::Vec2f lastClearPos2D(mLastCacheClearPosition.x(), mLastCacheClearPosition.y());
        float movementDistance = (currentPos2D - lastClearPos2D).length();

        // NEW: More frequent cache clearing (every 128 units instead of 256)
        // This ensures chunks update subdivision levels as player moves through them
        const float CACHE_CLEAR_THRESHOLD = 128.0f;

        if (movementDistance > CACHE_CLEAR_THRESHOLD)
        {
            // Clear the cache to force chunk recreation with new subdivisions
            clearCache();

            // Update the last clear position
            mLastCacheClearPosition = pos;
        }

        // Always update the current player position for new chunk creation
        mPlayerPosition = pos;
    }

    void ChunkManager::updateSubdivisionTracker(float dt)
    {
        if (mSubdivisionTracker)
        {
            osg::Vec2f playerPos2D(mPlayerPosition.x(), mPlayerPosition.y());
            mSubdivisionTracker->update(dt, playerPos2D);
        }
    }

    void ChunkManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Terrain Chunk", frameNumber, mCache->getStats(), *stats);
    }

    void ChunkManager::clearCache()
    {
        GenericResourceManager<ChunkKey>::clearCache();

        mBufferCache.clearCache();

        // Update last cache clear position to current player position
        // This prevents immediate re-clearing after manual cache clear
        mLastCacheClearPosition = mPlayerPosition;
    }

    void ChunkManager::releaseGLObjects(osg::State* state)
    {
        GenericResourceManager<ChunkKey>::releaseGLObjects(state);
        mBufferCache.releaseGLObjects(state);
    }

    osg::ref_ptr<osg::Texture2D> ChunkManager::createCompositeMapRTT()
    {
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D;
        texture->setTextureWidth(mCompositeMapSize);
        texture->setTextureHeight(mCompositeMapSize);
        texture->setInternalFormat(GL_RGB);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mSceneManager->applyFilterSettings(texture);

        return texture;
    }

    void ChunkManager::createCompositeMapGeometry(
        float chunkSize, const osg::Vec2f& chunkCenter, const osg::Vec4f& texCoords, CompositeMap& compositeMap)
    {
        if (chunkSize > mMaxCompGeometrySize)
        {
            createCompositeMapGeometry(chunkSize / 2.f, chunkCenter + osg::Vec2f(chunkSize / 4.f, chunkSize / 4.f),
                osg::Vec4f(
                    texCoords.x() + texCoords.z() / 2.f, texCoords.y(), texCoords.z() / 2.f, texCoords.w() / 2.f),
                compositeMap);
            createCompositeMapGeometry(chunkSize / 2.f, chunkCenter + osg::Vec2f(-chunkSize / 4.f, chunkSize / 4.f),
                osg::Vec4f(texCoords.x(), texCoords.y(), texCoords.z() / 2.f, texCoords.w() / 2.f), compositeMap);
            createCompositeMapGeometry(chunkSize / 2.f, chunkCenter + osg::Vec2f(chunkSize / 4.f, -chunkSize / 4.f),
                osg::Vec4f(texCoords.x() + texCoords.z() / 2.f, texCoords.y() + texCoords.w() / 2.f,
                    texCoords.z() / 2.f, texCoords.w() / 2.f),
                compositeMap);
            createCompositeMapGeometry(chunkSize / 2.f, chunkCenter + osg::Vec2f(-chunkSize / 4.f, -chunkSize / 4.f),
                osg::Vec4f(
                    texCoords.x(), texCoords.y() + texCoords.w() / 2.f, texCoords.z() / 2.f, texCoords.w() / 2.f),
                compositeMap);
        }
        else
        {
            float left = texCoords.x() * 2.f - 1;
            float top = texCoords.y() * 2.f - 1;
            float width = texCoords.z() * 2.f;
            float height = texCoords.w() * 2.f;

            std::vector<osg::ref_ptr<osg::StateSet>> passes = createPasses(chunkSize, chunkCenter, true);
            for (std::vector<osg::ref_ptr<osg::StateSet>>::iterator it = passes.begin(); it != passes.end(); ++it)
            {
                osg::ref_ptr<osg::Geometry> geom = osg::createTexturedQuadGeometry(
                    osg::Vec3(left, top, 0), osg::Vec3(width, 0, 0), osg::Vec3(0, height, 0));
                geom->setUseDisplayList(
                    false); // don't bother making a display list for an object that is just rendered once.
                geom->setUseVertexBufferObjects(false);
                geom->setTexCoordArray(1, geom->getTexCoordArray(0), osg::Array::BIND_PER_VERTEX);

                geom->setStateSet(*it);

                compositeMap.mDrawables.emplace_back(geom);
            }
        }
    }

    std::vector<osg::ref_ptr<osg::StateSet>> ChunkManager::createPasses(
        float chunkSize, const osg::Vec2f& chunkCenter, bool forCompositeMap)
    {
        std::vector<LayerInfo> layerList;
        std::vector<osg::ref_ptr<osg::Image>> blendmaps;
        mStorage->getBlendmaps(chunkSize, chunkCenter, blendmaps, layerList, mWorldspace);

        bool useShaders = mSceneManager->getForceShaders();
        if (!mSceneManager->getClampLighting())
            useShaders = true; // always use shaders when lighting is unclamped, this is to avoid lighting seams between
                               // a terrain chunk with normal maps and one without normal maps

        std::vector<TextureLayer> layers;
        {
            for (std::vector<LayerInfo>::const_iterator it = layerList.begin(); it != layerList.end(); ++it)
            {
                TextureLayer textureLayer;
                textureLayer.mParallax = it->mParallax;
                textureLayer.mSpecular = it->mSpecular;

                textureLayer.mDiffuseMap = mTextureManager->getTexture(it->mDiffuseMap);

                if (!forCompositeMap && !it->mNormalMap.empty())
                    textureLayer.mNormalMap = mTextureManager->getTexture(it->mNormalMap);

                if (it->requiresShaders())
                    useShaders = true;

                layers.push_back(textureLayer);
            }
        }

        if (forCompositeMap)
            useShaders = false;

        std::vector<osg::ref_ptr<osg::Texture2D>> blendmapTextures;
        for (std::vector<osg::ref_ptr<osg::Image>>::const_iterator it = blendmaps.begin(); it != blendmaps.end(); ++it)
        {
            osg::ref_ptr<osg::Texture2D> texture(new osg::Texture2D);
            texture->setImage(*it);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            texture->setResizeNonPowerOfTwoHint(false);
            blendmapTextures.push_back(texture);
        }

        float tileCount = mStorage->getTextureTileCount(chunkSize, mWorldspace);

        return ::Terrain::createPasses(
            useShaders, mSceneManager, layers, blendmapTextures, tileCount, tileCount, ESM::isEsm4Ext(mWorldspace));
    }

    osg::ref_ptr<osg::Node> ChunkManager::createChunk(float chunkSize, const osg::Vec2f& chunkCenter, unsigned char lod,
        unsigned int lodFlags, bool compile, const TerrainDrawable* templateGeometry, const osg::Vec3f& viewPoint)
    {
        osg::ref_ptr<TerrainDrawable> geometry(new TerrainDrawable);

        if (!templateGeometry)
        {
            osg::ref_ptr<osg::Vec3Array> positions(new osg::Vec3Array);
            osg::ref_ptr<osg::Vec3Array> normals(new osg::Vec3Array);
            osg::ref_ptr<osg::Vec4ubArray> colors(new osg::Vec4ubArray);
            colors->setNormalize(true);

            mStorage->fillVertexBuffers(lod, chunkSize, chunkCenter, mWorldspace, *positions, *normals, *colors);

            osg::ref_ptr<osg::VertexBufferObject> vbo(new osg::VertexBufferObject);
            positions->setVertexBufferObject(vbo);
            normals->setVertexBufferObject(vbo);
            colors->setVertexBufferObject(vbo);

            geometry->setVertexArray(positions);
            geometry->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
            geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
        }
        else
        {
            // Unfortunately we need to copy vertex data because of poor coupling with VertexBufferObject.
            osg::ref_ptr<osg::Array> positions
                = static_cast<osg::Array*>(templateGeometry->getVertexArray()->clone(osg::CopyOp::DEEP_COPY_ALL));
            osg::ref_ptr<osg::Array> normals
                = static_cast<osg::Array*>(templateGeometry->getNormalArray()->clone(osg::CopyOp::DEEP_COPY_ALL));
            osg::ref_ptr<osg::Array> colors
                = static_cast<osg::Array*>(templateGeometry->getColorArray()->clone(osg::CopyOp::DEEP_COPY_ALL));

            osg::ref_ptr<osg::VertexBufferObject> vbo(new osg::VertexBufferObject);
            positions->setVertexBufferObject(vbo);
            normals->setVertexBufferObject(vbo);
            colors->setVertexBufferObject(vbo);

            geometry->setVertexArray(positions);
            geometry->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
            geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
        }

        geometry->setUseDisplayList(false);
        geometry->setUseVertexBufferObjects(true);

        if (chunkSize <= 1.f)
            geometry->setLightListCallback(new SceneUtil::LightListCallback);

        unsigned int numVerts = (mStorage->getCellVertices(mWorldspace) - 1) * chunkSize / (1 << lod) + 1;

        geometry->addPrimitiveSet(mBufferCache.getIndexBuffer(numVerts, lodFlags));

        bool useCompositeMap = chunkSize >= mCompositeMapLevel;
        unsigned int numUvSets = useCompositeMap ? 1 : 2;

        geometry->setTexCoordArrayList(osg::Geometry::ArrayList(numUvSets, mBufferCache.getUVBuffer(numVerts)));

        geometry->createClusterCullingCallback();

        // Create a chunk-specific stateset that inherits from mMultiPassRoot
        osg::ref_ptr<osg::StateSet> chunkStateSet = new osg::StateSet(*mMultiPassRoot, osg::CopyOp::SHALLOW_COPY);

        // Set chunk world offset uniform for snow deformation coordinate conversion
        // Vertices in the chunk are relative to chunkCenter, so this converts local->world
        // OpenMW terrain uses X=East, Y=North, Z=Up
        // chunkCenter is in cell coordinates (e.g., 0.5, 1.5), must convert to world units
        // Multiply by cellSize (typically 8192) to get world coordinates
        float cellSize = mStorage->getCellWorldSize(mWorldspace);
        osg::Vec3f chunkWorldOffset(chunkCenter.x() * cellSize, chunkCenter.y() * cellSize, 0.0f);
        chunkStateSet->addUniform(new osg::Uniform("chunkWorldOffset", chunkWorldOffset));

        geometry->setStateSet(chunkStateSet);

        if (templateGeometry)
        {
            if (templateGeometry->getCompositeMap())
            {
                geometry->setCompositeMap(templateGeometry->getCompositeMap());
                geometry->setCompositeMapRenderer(mCompositeMapRenderer);
            }
            geometry->setPasses(templateGeometry->getPasses());
        }
        else
        {
            if (useCompositeMap)
            {
                osg::ref_ptr<CompositeMap> compositeMap = new CompositeMap;
                compositeMap->mTexture = createCompositeMapRTT();

                createCompositeMapGeometry(chunkSize, chunkCenter, osg::Vec4f(0, 0, 1, 1), *compositeMap);

                mCompositeMapRenderer->addCompositeMap(compositeMap.get(), false);

                geometry->setCompositeMap(compositeMap);
                geometry->setCompositeMapRenderer(mCompositeMapRenderer);

                TextureLayer layer;
                layer.mDiffuseMap = compositeMap->mTexture;
                layer.mParallax = false;
                layer.mSpecular = false;
                geometry->setPasses(::Terrain::createPasses(
                    mSceneManager->getForceShaders() || !mSceneManager->getClampLighting(), mSceneManager,
                    std::vector<TextureLayer>(1, layer), std::vector<osg::ref_ptr<osg::Texture2D>>(), 1.f, 1.f));
            }
            else
            {
                geometry->setPasses(createPasses(chunkSize, chunkCenter, false));
            }
        }

        geometry->setupWaterBoundingBox(-1, chunkSize * mStorage->getCellWorldSize(mWorldspace) / numVerts);

        if (!templateGeometry && compile && mSceneManager->getIncrementalCompileOperation())
        {
            mSceneManager->getIncrementalCompileOperation()->add(geometry);
        }
        geometry->setNodeMask(mNodeMask);

        // SNOW DEFORMATION: Subdivide terrain near player for better deformation quality
        // Calculate distance from player to CHUNK EDGE (not center) for pre-subdivision

        // Convert chunk center from cell units to world units
        // chunkCenter is in cell coordinates (e.g., 0.5, 0.5 = center of cell 0,0)
        osg::Vec2f worldChunkCenter2D(chunkCenter.x() * cellSize, chunkCenter.y() * cellSize);

        // Get player's horizontal position (x=east-west, y=north-south in world units)
        // Note: OSG uses (x,y,z) = (east-west, north-south, height)
        osg::Vec2f playerPos2D(mPlayerPosition.x(), mPlayerPosition.y());

        // Calculate chunk half-size for edge distance calculation
        float halfChunkSize = chunkSize * cellSize * 0.5f;

        // Calculate distance to nearest chunk edge (not center!)
        // This allows pre-subdivision before player actually enters the chunk
        float distanceToEdgeX = std::max(0.0f, std::abs(playerPos2D.x() - worldChunkCenter2D.x()) - halfChunkSize);
        float distanceToEdgeY = std::max(0.0f, std::abs(playerPos2D.y() - worldChunkCenter2D.y()) - halfChunkSize);
        float distanceToEdge = std::sqrt(distanceToEdgeX * distanceToEdgeX + distanceToEdgeY * distanceToEdgeY);

        // Also calculate distance to center for reference/logging
        float distanceToCenter = (playerPos2D - worldChunkCenter2D).length();

        // Use distance to edge for subdivision decisions (allows pre-subdivision)
        float distance = distanceToEdge;

        // Calculate chunk bounding box for debugging (halfChunkSize already calculated above)
        float minX = worldChunkCenter2D.x() - halfChunkSize;
        float maxX = worldChunkCenter2D.x() + halfChunkSize;
        float minY = worldChunkCenter2D.y() - halfChunkSize;
        float maxY = worldChunkCenter2D.y() + halfChunkSize;

        // Check if player is inside this chunk
        bool playerInChunk = (mPlayerPosition.x() >= minX && mPlayerPosition.x() <= maxX &&
                             mPlayerPosition.y() >= minY && mPlayerPosition.y() <= maxY);

        // NEW: Use subdivision tracker to determine level (creates trail effect)
        // This consults both current distance AND historical subdivision state
        int subdivisionLevel = 0;
        if (chunkSize <= 1.0f && mSubdivisionTracker)
        {
            subdivisionLevel = mSubdivisionTracker->getSubdivisionLevel(chunkCenter, distance);
        }

        // Removed excessive debug logging

        // Always fetch terrain layer info for weight computation
        // The weight computation itself uses LOD to optimize distant chunks (LOD_NONE),
        // but we need the layer info to classify textures correctly.
        // This ensures chunks are created with correct weights even if player is far away,
        // preventing issues when cached chunks are reused as player approaches.
        std::vector<LayerInfo> layerList;
        std::vector<osg::ref_ptr<osg::Image>> blendmaps;
        mStorage->getBlendmaps(chunkSize, chunkCenter, blendmaps, layerList, mWorldspace);

        // Determine if this chunk needs full weight computation based on distance
        const float MAX_DEFORMATION_DISTANCE = 256.0f;
        bool needsWeightComputation = (distanceToCenter < MAX_DEFORMATION_DISTANCE * 1.5f); // 1.5x buffer for safety

        if (subdivisionLevel > 0)
        {
            // Subdivide geometry
            osg::ref_ptr<osg::Geometry> subdivided;

            if (needsWeightComputation)
            {
                // Close chunks: subdivide WITH terrain weight computation
                subdivided = TerrainSubdivider::subdivideWithWeights(
                    geometry.get(), subdivisionLevel,
                    chunkCenter, chunkSize,
                    layerList, blendmaps,
                    mStorage, mWorldspace,
                    mPlayerPosition, cellSize);
            }
            else
            {
                // Distant chunks: subdivide WITHOUT weight computation (faster)
                subdivided = TerrainSubdivider::subdivide(geometry.get(), subdivisionLevel);
            }

            if (subdivided)
            {
                // Copy TerrainDrawable-specific data to the subdivided geometry
                osg::ref_ptr<TerrainDrawable> subdividedDrawable = new TerrainDrawable;

                // Copy vertex data from subdivided geometry
                subdividedDrawable->setVertexArray(subdivided->getVertexArray());
                subdividedDrawable->setNormalArray(subdivided->getNormalArray(), osg::Array::BIND_PER_VERTEX);
                subdividedDrawable->setColorArray(subdivided->getColorArray(), osg::Array::BIND_PER_VERTEX);
                subdividedDrawable->setTexCoordArrayList(subdivided->getTexCoordArrayList());

                // Copy terrain weight vertex attribute (attribute 6)
                if (subdivided->getVertexAttribArray(6))
                    subdividedDrawable->setVertexAttribArray(6, subdivided->getVertexAttribArray(6), osg::Array::BIND_PER_VERTEX);

                // Copy primitive sets
                for (unsigned int i = 0; i < subdivided->getNumPrimitiveSets(); ++i)
                    subdividedDrawable->addPrimitiveSet(subdivided->getPrimitiveSet(i));

                // Copy TerrainDrawable-specific properties
                subdividedDrawable->setPasses(geometry->getPasses());
                subdividedDrawable->setCompositeMap(geometry->getCompositeMap());
                subdividedDrawable->setCompositeMapRenderer(mCompositeMapRenderer);
                subdividedDrawable->setStateSet(geometry->getStateSet());
                subdividedDrawable->setNodeMask(geometry->getNodeMask());
                subdividedDrawable->setUseDisplayList(false);
                subdividedDrawable->setUseVertexBufferObjects(true);

                // Set light list callback if this is a small chunk
                if (chunkSize <= 1.f)
                    subdividedDrawable->setLightListCallback(new SceneUtil::LightListCallback);

                subdividedDrawable->setupWaterBoundingBox(-1, chunkSize * mStorage->getCellWorldSize(mWorldspace) / numVerts);
                subdividedDrawable->createClusterCullingCallback();

                // Mark this chunk as subdivided in the tracker (for trail persistence)
                if (mSubdivisionTracker)
                {
                    mSubdivisionTracker->markChunkSubdivided(chunkCenter, subdivisionLevel, worldChunkCenter2D);
                }

                // Removed excessive subdivision logging

                return subdividedDrawable;
            }
            else
            {
                Log(Debug::Warning) << "[TERRAIN] Failed to subdivide chunk at (" << chunkCenter.x() << ", " << chunkCenter.y() << ")";
            }
        }
        else
        {
            // Non-subdivided chunk - ALWAYS add terrain weights
            // Even distant chunks need weights to correctly identify rock vs snow textures.
            // The weight computation uses LOD to optimize: distant chunks get LOD_NONE
            // which returns all-rock weights without expensive per-vertex sampling.
            osg::ref_ptr<osg::Geometry> weightedGeometry = TerrainSubdivider::subdivideWithWeights(
                geometry.get(), 0,  // subdivisionLevel = 0 (no subdivision, just add weights)
                chunkCenter, chunkSize,
                layerList, blendmaps,
                mStorage, mWorldspace,
                mPlayerPosition, cellSize);

            if (weightedGeometry && weightedGeometry->getVertexAttribArray(6))
            {
                // Attach the computed weights to the original geometry
                geometry->setVertexAttribArray(6, weightedGeometry->getVertexAttribArray(6), osg::Array::BIND_PER_VERTEX);

                Log(Debug::Verbose) << "[TERRAIN WEIGHTS] Added weights to non-subdivided chunk at ("
                                   << chunkCenter.x() << ", " << chunkCenter.y() << "), distance: "
                                   << distanceToCenter << "m";
            }
        }

        return geometry;
    }

}
