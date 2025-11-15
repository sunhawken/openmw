# Snow Deformation System - Implementation Plan (REVISED)

## Executive Summary

This document outlines the progressive implementation of a snow deformation system for OpenMW. The system will create realistic snow trails when the player character walks on snow-covered terrain. The implementation uses a **shader-based approach with dynamic mesh subdivision** and **texture-based detection** for maximum quality and flexibility.

**Key Revision**: This plan now includes dynamic mesh subdivision to address Morrowind's low-poly terrain, and uses texture-based snow detection instead of cell-based detection for universal compatibility.

## Architecture Overview

### Core Components

1. **Deformation Texture Manager** - Manages RTT textures that store deformation data
2. **Mesh Subdivision System** - Dynamically increases vertex density where deformation occurs
3. **Terrain Material Extension** - Modifies terrain shaders to sample and apply deformation
4. **Texture-Based Snow Detection** - Identifies snow textures at player position (not cell-based)
5. **Footprint Stamping System** - Projects player position into deformation texture
6. **Decay System** - Gradually restores snow surface over time
7. **Subdivision Cache Manager** - Manages subdivided chunk lifecycle and memory

### Key Design Decisions

- **Shader-based displacement**: Vertex positions modified in terrain.vert based on deformation texture
- **Dynamic mesh subdivision**: CPU-side triangle subdivision (2-3 levels) for terrain near player
- **Texture-based activation**: System activates when player is on snow texture (works anywhere)
- **World-space deformation texture**: Follows player, centered at player position
- **Fixed texture resolution**: 512x512 or 1024x1024 deformation map
- **Subdivision persistence**: Subdivided chunks persist until deformation fully decayed
- **Memory-limited**: Max 50-100 subdivided chunks (~115-230 MB VRAM)

## The Vertex Density Problem & Solution

### The Problem

**Morrowind terrain is extremely low-poly:**
- ~65 vertices per cell edge (64x64 grid)
- Cell size: 8192 units
- **Vertex spacing: ~128 units apart**
- Typical footprint radius: 24 units

**Result**: Only 1-2 vertices per footprint → blocky, unconvincing deformation

### The Solution: Dynamic Mesh Subdivision

**Triangle Subdivision Algorithm:**
```
Original triangle:
    v0
    /\
   /  \
  /____\
 v1    v2

After 1 level (4x triangles):
      v0
      /\
   v01/__\v20
    /\  /\
 v1/__\/___\v2
      v12

After 2 levels (16x triangles):
  [Each of the 4 becomes 4 more]

After 3 levels (64x triangles):
  [Each of the 16 becomes 4 more]
```

**Vertex Density Comparison:**
| Subdivision Level | Vertices per Chunk | Vertex Spacing | Memory/Chunk |
|-------------------|-------------------|----------------|--------------|
| 0 (original)      | 4,096 (64×64)    | 128 units     | 144 KB       |
| 1                 | 16,384 (128×128) | 64 units      | 576 KB       |
| 2                 | 65,536 (256×256) | 32 units      | 2.3 MB       |
| 3                 | 262,144 (512×512)| 16 units      | 9.2 MB       |

**Target**: Level 2 (32-unit spacing) gives 1-2 vertices per footprint edge = acceptable quality

### Subdivision Strategy

**Distance-based quality:**
```
Distance from Player:     Subdivision:    Vertex Spacing:
0-256 units              Level 2-3       16-32 units  (high quality)
256-512 units            Level 1-2       32-64 units  (medium)
512-1024 units           Level 0-1       64-128 units (low, blocky)
1024+ units              None            128 units    (no deformation)
```

**Chunk lifecycle:**
1. Player approaches → Subdivide chunk (if has snow texture)
2. Player stands/walks → Stamp footprints, deformation accumulates
3. Player leaves → Keep subdivided while deformation visible
4. Deformation decays → Release subdivision after full decay (2-5 minutes)
5. Memory full → Force-release oldest chunks

## Texture-Based Activation (Not Cell-Based!)

### Why Texture-Based?

**Problem with cell-based:**
- ❌ Some Solstheim cells have no snow (rock, dirt)
- ❌ Some Vvardenfell cells have snow patches
- ❌ Mods add snow anywhere
- ❌ Doesn't work with custom worldspaces

**Texture-based solution:**
- ✅ Works anywhere snow texture exists
- ✅ Works in vanilla, expansions, and mods
- ✅ Seamless transitions at texture boundaries
- ✅ No manual region configuration needed

### Implementation

**Check texture under player's feet:**
```cpp
bool SnowDeformationManager::shouldBeActive(const osg::Vec3f& worldPos) {
    // 1. Get terrain chunk at player position
    // 2. Query texture layers for that chunk
    // 3. Sample blendmaps at player's UV coordinate
    // 4. Check if snow texture has sufficient blend weight (>10%)
    // 5. Return true if standing on snow

    TerrainStorage* storage = mTerrainWorld->getStorage();

    // Convert world position to cell + UV
    int cellX = std::floor(worldPos.x() / 8192.0f);
    int cellZ = std::floor(worldPos.z() / 8192.0f);
    osg::Vec2f uv = getCellLocalUV(worldPos, cellX, cellZ);

    // Get layers and check for snow
    std::vector<LayerInfo> layers;
    storage->getLayersAtPosition(cellX, cellZ, uv, layers);

    for (const auto& layer : layers) {
        if (isSnowTexture(layer.diffuseMap)) {
            float blendWeight = sampleBlendMap(layer.blendMap, uv);
            if (blendWeight > 0.1f) {
                return true; // Standing on snow!
            }
        }
    }

    return false; // Not on snow
}
```

**Snow texture detection patterns:**
- Filename contains: "snow", "ice", "frost"
- Common Morrowind: `tx_snow_*.dds`, `tx_bc_snow*.dds`, `tx_ice_*.dds`
- Configurable via settings file
- Case-insensitive matching

## Technical Constraints & Challenges

### OpenMW Terrain System Characteristics

1. **Cell-based architecture**: Terrain divided into 8192-unit cells
2. **Quad-tree LOD**: Dynamic level of detail based on distance
3. **Multi-pass rendering**: Terrain uses multiple passes for texture layering
4. **Shared vertex buffers**: Geometry shared across chunks via BufferCache
5. **Composite maps**: Distant terrain uses baked textures
6. **Low vertex density**: ~128 units between vertices (requires subdivision)

### Coordinate System Complexity

```
World Space (OSG):     Cell Coordinates:      Chunk Local Space:
- X: East              - (x,y) integers       - Origin at chunk center
- Y: Up                - Cell size: 8192      - X: East (relative)
- Z: North             - Example: (0,0)       - Y: Up (height)
                         = [0-8192, 0-8192]   - Z: North (relative)

Deformation Texture Space:
- Center follows player in XZ plane
- Fixed world coverage (e.g., 300 units radius)
- UV mapping: world XZ → texture UV

Subdivision Space:
- Operates in chunk local space
- Creates new vertices via midpoint interpolation
- Preserves UV coordinates and normals
```

### Integration Challenges

1. **Multiple terrain chunks**: Deformation must be consistent across chunk boundaries
2. **LOD transitions**: Deformation quality should degrade gracefully with distance
3. **Composite map compatibility**: Distant terrain won't show deformation (acceptable trade-off)
4. **Performance**: Deformation + subdivision must be fast (target: <2ms per frame)
5. **Memory management**: Limit subdivided chunks to prevent VRAM exhaustion
6. **Chunk lifecycle**: Keep subdivided geometry until deformation fully decayed
7. **Subdivision seams**: Prevent cracks between subdivided and non-subdivided chunks

## Implementation Phases

---

## PHASE 0: Mesh Subdivision Foundation

**Goal**: Create the triangle subdivision system before anything else

**Why First**: This is the foundation that makes visible deformation possible. Without it, everything else is pointless due to Morrowind's low-poly terrain.

### Tasks

#### 0.1 Create TerrainSubdivider Utility Class

**File**: `components/terrain/terrainsubdivider.hpp` + `.cpp`

**Class Structure**:
```cpp
namespace Terrain {

class TerrainSubdivider {
public:
    // Main subdivision function
    // levels: 1=4x tris, 2=16x tris, 3=64x tris
    static osg::ref_ptr<osg::Geometry> subdivide(
        const osg::Geometry* source,
        int levels
    );

private:
    // Process different primitive types
    static void subdivideTriangles(
        const osg::DrawElements* primitives,
        const osg::Vec3Array* srcVerts,
        const osg::Vec3Array* srcNormals,
        const osg::Vec2Array* srcUVs,
        const osg::Vec4ubArray* srcColors,
        osg::Vec3Array* dstVerts,
        osg::Vec3Array* dstNormals,
        osg::Vec2Array* dstUVs,
        osg::Vec4ubArray* dstColors,
        int levels
    );

    // Recursive subdivision of a single triangle
    static void subdivideTriangleRecursive(
        const osg::Vec3& v0, const osg::Vec3& v1, const osg::Vec3& v2,
        const osg::Vec3& n0, const osg::Vec3& n1, const osg::Vec3& n2,
        const osg::Vec2& uv0, const osg::Vec2& uv1, const osg::Vec2& uv2,
        const osg::Vec4ub& c0, const osg::Vec4ub& c1, const osg::Vec4ub& c2,
        osg::Vec3Array* dstV, osg::Vec3Array* dstN,
        osg::Vec2Array* dstUV, osg::Vec4ubArray* dstC,
        int level
    );

    // Interpolation helpers
    static osg::Vec3 interpolateNormal(const osg::Vec3& n0, const osg::Vec3& n1);
    static osg::Vec4ub interpolateColor(const osg::Vec4ub& c0, const osg::Vec4ub& c1);
};

} // namespace Terrain
```

**Implementation Details**:

```cpp
// terrainsubdivider.cpp

osg::ref_ptr<osg::Geometry> TerrainSubdivider::subdivide(
    const osg::Geometry* source, int levels) {

    if (levels == 0 || !source) {
        return osg::clone(source, osg::CopyOp::DEEP_COPY_ALL);
    }

    osg::ref_ptr<osg::Geometry> result = new osg::Geometry;

    // Get source arrays
    const osg::Vec3Array* srcVerts =
        static_cast<const osg::Vec3Array*>(source->getVertexArray());
    const osg::Vec3Array* srcNormals =
        static_cast<const osg::Vec3Array*>(source->getNormalArray());
    const osg::Vec2Array* srcUVs =
        static_cast<const osg::Vec2Array*>(source->getTexCoordArray(0));
    const osg::Vec4ubArray* srcColors =
        static_cast<const osg::Vec4ubArray*>(source->getColorArray());

    if (!srcVerts || !srcNormals || !srcUVs) {
        OSG_WARN << "TerrainSubdivider: Missing required arrays" << std::endl;
        return nullptr;
    }

    // Create destination arrays
    osg::ref_ptr<osg::Vec3Array> dstVerts = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec3Array> dstNormals = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec2Array> dstUVs = new osg::Vec2Array;
    osg::ref_ptr<osg::Vec4ubArray> dstColors = srcColors ? new osg::Vec4ubArray : nullptr;

    // Reserve space (rough estimate)
    size_t estimatedVerts = srcVerts->size() * std::pow(4, levels);
    dstVerts->reserve(estimatedVerts);
    dstNormals->reserve(estimatedVerts);
    dstUVs->reserve(estimatedVerts);
    if (dstColors) dstColors->reserve(estimatedVerts);

    // Process each primitive set
    for (unsigned int i = 0; i < source->getNumPrimitiveSets(); ++i) {
        const osg::PrimitiveSet* ps = source->getPrimitiveSet(i);

        if (ps->getMode() == GL_TRIANGLES) {
            const osg::DrawElements* de = dynamic_cast<const osg::DrawElements*>(ps);
            if (de) {
                subdivideTriangles(de, srcVerts, srcNormals, srcUVs, srcColors,
                                  dstVerts.get(), dstNormals.get(),
                                  dstUVs.get(), dstColors.get(), levels);
            }
        }
        // TODO: Handle GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN if needed
    }

    // Set arrays on result geometry
    result->setVertexArray(dstVerts);
    result->setNormalArray(dstNormals, osg::Array::BIND_PER_VERTEX);
    result->setTexCoordArray(0, dstUVs);
    if (dstColors) {
        result->setColorArray(dstColors, osg::Array::BIND_PER_VERTEX);
    }

    // Single triangle list primitive
    result->addPrimitiveSet(
        new osg::DrawArrays(GL_TRIANGLES, 0, dstVerts->size()));

    // Copy state set from source
    result->setStateSet(source->getStateSet());

    return result;
}

void TerrainSubdivider::subdivideTriangles(
    const osg::DrawElements* primitives,
    const osg::Vec3Array* srcV, const osg::Vec3Array* srcN,
    const osg::Vec2Array* srcUV, const osg::Vec4ubArray* srcC,
    osg::Vec3Array* dstV, osg::Vec3Array* dstN,
    osg::Vec2Array* dstUV, osg::Vec4ubArray* dstC,
    int levels) {

    // Process triangles in groups of 3 indices
    for (unsigned int i = 0; i < primitives->getNumIndices(); i += 3) {
        unsigned int i0 = primitives->index(i);
        unsigned int i1 = primitives->index(i+1);
        unsigned int i2 = primitives->index(i+2);

        subdivideTriangleRecursive(
            (*srcV)[i0], (*srcV)[i1], (*srcV)[i2],
            (*srcN)[i0], (*srcN)[i1], (*srcN)[i2],
            (*srcUV)[i0], (*srcUV)[i1], (*srcUV)[i2],
            srcC ? (*srcC)[i0] : osg::Vec4ub(255,255,255,255),
            srcC ? (*srcC)[i1] : osg::Vec4ub(255,255,255,255),
            srcC ? (*srcC)[i2] : osg::Vec4ub(255,255,255,255),
            dstV, dstN, dstUV, dstC, levels
        );
    }
}

void TerrainSubdivider::subdivideTriangleRecursive(
    const osg::Vec3& v0, const osg::Vec3& v1, const osg::Vec3& v2,
    const osg::Vec3& n0, const osg::Vec3& n1, const osg::Vec3& n2,
    const osg::Vec2& uv0, const osg::Vec2& uv1, const osg::Vec2& uv2,
    const osg::Vec4ub& c0, const osg::Vec4ub& c1, const osg::Vec4ub& c2,
    osg::Vec3Array* dstV, osg::Vec3Array* dstN,
    osg::Vec2Array* dstUV, osg::Vec4ubArray* dstC,
    int level) {

    if (level == 0) {
        // Base case: add triangle
        dstV->push_back(v0);
        dstV->push_back(v1);
        dstV->push_back(v2);

        dstN->push_back(n0);
        dstN->push_back(n1);
        dstN->push_back(n2);

        dstUV->push_back(uv0);
        dstUV->push_back(uv1);
        dstUV->push_back(uv2);

        if (dstC) {
            dstC->push_back(c0);
            dstC->push_back(c1);
            dstC->push_back(c2);
        }
        return;
    }

    // Calculate midpoints
    osg::Vec3 v01 = (v0 + v1) * 0.5f;
    osg::Vec3 v12 = (v1 + v2) * 0.5f;
    osg::Vec3 v20 = (v2 + v0) * 0.5f;

    osg::Vec3 n01 = interpolateNormal(n0, n1);
    osg::Vec3 n12 = interpolateNormal(n1, n2);
    osg::Vec3 n20 = interpolateNormal(n2, n0);

    osg::Vec2 uv01 = (uv0 + uv1) * 0.5f;
    osg::Vec2 uv12 = (uv1 + uv2) * 0.5f;
    osg::Vec2 uv20 = (uv2 + uv0) * 0.5f;

    osg::Vec4ub c01 = interpolateColor(c0, c1);
    osg::Vec4ub c12 = interpolateColor(c1, c2);
    osg::Vec4ub c20 = interpolateColor(c2, c0);

    // Recurse on 4 sub-triangles
    //      v0
    //      /\
    //   v01/__\v20
    //    /\  /\
    // v1/__\/___\v2
    //      v12

    subdivideTriangleRecursive(v0, v01, v20, n0, n01, n20,
                               uv0, uv01, uv20, c0, c01, c20,
                               dstV, dstN, dstUV, dstC, level - 1);

    subdivideTriangleRecursive(v01, v1, v12, n01, n1, n12,
                               uv01, uv1, uv12, c01, c1, c12,
                               dstV, dstN, dstUV, dstC, level - 1);

    subdivideTriangleRecursive(v20, v12, v2, n20, n12, n2,
                               uv20, uv12, uv2, c20, c12, c2,
                               dstV, dstN, dstUV, dstC, level - 1);

    subdivideTriangleRecursive(v01, v12, v20, n01, n12, n20,
                               uv01, uv12, uv20, c01, c12, c20,
                               dstV, dstN, dstUV, dstC, level - 1);
}

osg::Vec3 TerrainSubdivider::interpolateNormal(
    const osg::Vec3& n0, const osg::Vec3& n1) {
    osg::Vec3 result = (n0 + n1);
    result.normalize();
    return result;
}

osg::Vec4ub TerrainSubdivider::interpolateColor(
    const osg::Vec4ub& c0, const osg::Vec4ub& c1) {
    return osg::Vec4ub(
        (c0.r() + c1.r()) / 2,
        (c0.g() + c1.g()) / 2,
        (c0.b() + c1.b()) / 2,
        (c0.a() + c1.a()) / 2
    );
}
```

**Success Criteria**:
- Compiles without errors
- Subdivides simple test geometry correctly
- Vertices, normals, UVs, colors all interpolated properly
- No memory leaks
- Performance acceptable (~5-10ms per chunk for level 2)

#### 0.2 Create Simple Test Program

**File**: `apps/openmw_test_subdivision/main.cpp` (standalone test)

**Purpose**: Validate subdivision works before integrating with terrain

```cpp
#include <components/terrain/terrainsubdivider.hpp>
#include <osg/Geometry>
#include <osg/Vec3>
#include <iostream>

int main() {
    // Create simple triangle
    osg::ref_ptr<osg::Geometry> triangle = new osg::Geometry;

    osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array;
    verts->push_back(osg::Vec3(0, 0, 0));
    verts->push_back(osg::Vec3(100, 0, 0));
    verts->push_back(osg::Vec3(50, 0, 100));

    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
    normals->push_back(osg::Vec3(0, 1, 0));
    normals->push_back(osg::Vec3(0, 1, 0));
    normals->push_back(osg::Vec3(0, 1, 0));

    osg::ref_ptr<osg::Vec2Array> uvs = new osg::Vec2Array;
    uvs->push_back(osg::Vec2(0, 0));
    uvs->push_back(osg::Vec2(1, 0));
    uvs->push_back(osg::Vec2(0.5, 1));

    triangle->setVertexArray(verts);
    triangle->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
    triangle->setTexCoordArray(0, uvs);
    triangle->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));

    // Test subdivision levels
    for (int level = 0; level <= 3; ++level) {
        osg::ref_ptr<osg::Geometry> subdivided =
            Terrain::TerrainSubdivider::subdivide(triangle.get(), level);

        const osg::Vec3Array* resultVerts =
            static_cast<const osg::Vec3Array*>(subdivided->getVertexArray());

        size_t expectedVerts = 3 * std::pow(4, level);
        size_t actualVerts = resultVerts->size();

        std::cout << "Level " << level << ": "
                  << "Expected " << expectedVerts << " verts, "
                  << "Got " << actualVerts << " verts - "
                  << (actualVerts == expectedVerts ? "PASS" : "FAIL")
                  << std::endl;
    }

    return 0;
}
```

**Expected Output**:
```
Level 0: Expected 3 verts, Got 3 verts - PASS
Level 1: Expected 12 verts, Got 12 verts - PASS
Level 2: Expected 48 verts, Got 48 verts - PASS
Level 3: Expected 192 verts, Got 192 verts - PASS
```

**Success Criteria**:
- Test program compiles
- All subdivision levels produce correct vertex count
- Visual inspection of subdivided mesh (optional)

### Testing Phase 0

**Test Scenario 1**: Unit Test
- Run test program
- Verify vertex counts match expected
- Verify no crashes or assertions

**Test Scenario 2**: Memory Test
- Subdivide large geometry (10k triangles)
- Monitor memory usage
- Verify no leaks after subdivision

**Test Scenario 3**: Visual Test (Optional)
- Export subdivided geometry to OBJ file
- View in 3D viewer (Blender, etc.)
- Verify smooth interpolation, no cracks

**Deliverables**:
- TerrainSubdivider class (fully functional)
- Unit test program
- Performance baseline measurements
- CMakeLists.txt updated

---

## PHASE 1: Foundation & Texture-Based Snow Detection

**Goal**: Identify snow textures at runtime based on player position

### Tasks

#### 1.1 Create Snow Detection Utilities

**File**: `components/terrain/snowdetection.hpp` + `.cpp`

**Purpose**: Determine if player is standing on snow texture

```cpp
namespace Terrain {

class SnowDetection {
public:
    // Check if a texture filename indicates snow
    static bool isSnowTexture(const std::string& texturePath);

    // Check if terrain at world position has snow
    static bool hasSnowAtPosition(
        const osg::Vec3f& worldPos,
        Storage* terrainStorage,
        ESM::RefId worldspace
    );

    // Sample blendmap to get texture weight at UV coordinate
    static float sampleBlendMap(
        const osg::Image* blendmap,
        const osg::Vec2f& uv
    );

    // Load snow texture patterns from settings
    static void loadSnowPatterns();

private:
    static std::vector<std::string> sSnowPatterns;
    static bool sPatternsLoaded;
};

} // namespace Terrain
```

**Implementation**:

```cpp
// snowdetection.cpp

#include "snowdetection.hpp"
#include <components/settings/settings.hpp>
#include <algorithm>
#include <cctype>

std::vector<std::string> SnowDetection::sSnowPatterns;
bool SnowDetection::sPatternsLoaded = false;

void SnowDetection::loadSnowPatterns() {
    if (sPatternsLoaded) return;

    // Default patterns
    sSnowPatterns = {
        "snow", "ice", "frost", "glacier",
        "tx_snow", "tx_bc_snow", "tx_ice"
    };

    // TODO: Load additional patterns from settings
    // std::string customPatterns = Settings::Manager::getString(
    //     "snow texture patterns", "Terrain");

    sPatternsLoaded = true;
}

bool SnowDetection::isSnowTexture(const std::string& texturePath) {
    loadSnowPatterns();

    // Convert to lowercase for case-insensitive comparison
    std::string lowerPath = texturePath;
    std::transform(lowerPath.begin(), lowerPath.end(),
                   lowerPath.begin(), ::tolower);

    // Check if any pattern matches
    for (const auto& pattern : sSnowPatterns) {
        if (lowerPath.find(pattern) != std::string::npos) {
            return true;
        }
    }

    return false;
}

bool SnowDetection::hasSnowAtPosition(
    const osg::Vec3f& worldPos,
    Storage* terrainStorage,
    ESM::RefId worldspace) {

    if (!terrainStorage) return false;

    // Convert world position to cell coordinates
    float cellSize = terrainStorage->getCellWorldSize(worldspace);
    int cellX = static_cast<int>(std::floor(worldPos.x() / cellSize));
    int cellZ = static_cast<int>(std::floor(worldPos.z() / cellSize));

    // Get local UV within cell (0-1 range)
    float localX = worldPos.x() - (cellX * cellSize);
    float localZ = worldPos.z() - (cellZ * cellSize);
    osg::Vec2f cellUV(localX / cellSize, localZ / cellSize);

    // Get texture layers for this chunk
    // Note: This is a simplified version - real implementation
    // needs to query the actual chunk and its blendmaps

    // TODO: Implement actual layer querying
    // For now, return false as placeholder
    // Real implementation in Phase 1.2

    return false;
}

float SnowDetection::sampleBlendMap(
    const osg::Image* blendmap,
    const osg::Vec2f& uv) {

    if (!blendmap) return 0.0f;

    // Clamp UV to [0, 1]
    float u = std::max(0.0f, std::min(1.0f, uv.x()));
    float v = std::max(0.0f, std::min(1.0f, uv.y()));

    // Convert to pixel coordinates
    int x = static_cast<int>(u * (blendmap->s() - 1));
    int y = static_cast<int>(v * (blendmap->t() - 1));

    // Sample alpha channel (blend weight)
    // Assuming RGBA format
    const unsigned char* data = blendmap->data(x, y);
    float alpha = data[3] / 255.0f; // Alpha channel

    return alpha;
}
```

**Success Criteria**:
- isSnowTexture() correctly identifies snow textures
- Case-insensitive matching works
- Configurable patterns (via settings or hardcoded initially)

#### 1.2 Create Deformation Manager Class (Skeleton)

**File**: `components/terrain/snowdeformation.hpp` + `.cpp`

**Class Structure** (minimal for Phase 1):
```cpp
namespace Terrain {

class SnowDeformationManager {
public:
    SnowDeformationManager(
        Resource::SceneManager* sceneManager,
        Storage* terrainStorage
    );
    ~SnowDeformationManager();

    // Check if system should be active at this position
    bool shouldBeActive(const osg::Vec3f& worldPos);

    // Enable/disable system
    void setEnabled(bool enabled);
    bool isEnabled() const { return mEnabled; }

    // Settings
    void setWorldspace(ESM::RefId worldspace);

private:
    Resource::SceneManager* mSceneManager;
    Storage* mTerrainStorage;
    ESM::RefId mWorldspace;
    bool mEnabled;
};

} // namespace Terrain
```

**Implementation** (Phase 1 skeleton):
```cpp
// snowdeformation.cpp

#include "snowdeformation.hpp"
#include "snowdetection.hpp"
#include <components/debug/debuglog.hpp>

namespace Terrain {

SnowDeformationManager::SnowDeformationManager(
    Resource::SceneManager* sceneManager,
    Storage* terrainStorage)
    : mSceneManager(sceneManager)
    , mTerrainStorage(terrainStorage)
    , mWorldspace(ESM::RefId())
    , mEnabled(true)
{
    Log(Debug::Info) << "SnowDeformationManager created";
}

SnowDeformationManager::~SnowDeformationManager() {
    Log(Debug::Info) << "SnowDeformationManager destroyed";
}

bool SnowDeformationManager::shouldBeActive(const osg::Vec3f& worldPos) {
    if (!mEnabled) return false;

    // Check if player is on snow texture
    bool onSnow = SnowDetection::hasSnowAtPosition(
        worldPos, mTerrainStorage, mWorldspace);

    if (onSnow) {
        Log(Debug::Verbose) << "Player on snow at " << worldPos.x()
                           << ", " << worldPos.z();
    }

    return onSnow;
}

void SnowDeformationManager::setEnabled(bool enabled) {
    if (mEnabled != enabled) {
        Log(Debug::Info) << "Snow deformation "
                        << (enabled ? "enabled" : "disabled");
        mEnabled = enabled;
    }
}

void SnowDeformationManager::setWorldspace(ESM::RefId worldspace) {
    mWorldspace = worldspace;
}

} // namespace Terrain
```

**Success Criteria**:
- Class compiles and instantiates
- shouldBeActive() can be called without crashing
- Logging works

#### 1.3 Settings Configuration

**File**: `files/settings-default.cfg`

**Add new section**:
```ini
[Terrain]

# Snow Deformation System
# Enable dynamic snow deformation (footprints, trails)
snow deformation = true

# Deformation texture resolution (256, 512, or 1024)
# Higher = better quality, more VRAM
snow deformation resolution = 512

# World-space radius covered by deformation texture (in game units)
# Default: 150 units (~30 meters)
snow deformation radius = 150.0

# Maximum distance to apply deformation (LOD cutoff)
snow deformation max distance = 1024.0

# Mesh Subdivision Settings
# Subdivision level for terrain near player (0-3)
# 0=no subdivision, 1=4x density, 2=16x density, 3=64x density
snow subdivision level close = 2    # 0-256 units
snow subdivision level medium = 1   # 256-512 units
snow subdivision level far = 0      # 512-1024 units

# Maximum number of subdivided chunks to keep in memory
# Each subdivided chunk uses ~2-3 MB VRAM at level 2
snow max subdivided chunks = 50

# Footprint Settings
# Radius of each footprint (in game units)
snow footprint radius = 24.0

# Distance player must move before new footprint stamped
snow footprint interval = 15.0

# Maximum depth of deformation (in game units, downward)
snow deformation depth = 8.0

# Snow Decay (Restoration)
# Enable gradual restoration of snow surface
snow decay enabled = true

# Time for full restoration (in seconds)
snow decay time = 120.0

# Decay curve type: linear, exponential, or smooth
snow decay curve = linear

# Snow Texture Detection
# Additional texture name patterns (comma-separated)
# These are case-insensitive substrings
snow texture patterns = snow,ice,frost,glacier

# Minimum blend weight to activate deformation (0.0-1.0)
# Set higher to avoid deformation on barely-visible snow textures
snow blend threshold = 0.1

# Debug Options
# Show deformation texture overlay on screen
snow debug visualization = false

# Log snow detection events
snow debug logging = false
```

**Success Criteria**:
- Settings parse correctly
- Values accessible via Settings::Manager
- Invalid values clamped to safe ranges

### Testing Phase 1

**Test Scenario 1**: Snow Texture Detection
- Identify common Morrowind snow textures
- Test isSnowTexture() with various filenames
- Verify case-insensitive matching

**Test Scenario 2**: Manager Lifecycle
- Instantiate SnowDeformationManager
- Call shouldBeActive() at various positions
- Verify no crashes
- Check debug logging output

**Test Scenario 3**: Settings
- Load settings from file
- Verify defaults
- Try invalid values (negative, huge numbers)
- Verify clamping works

**Deliverables**:
- SnowDetection utilities
- SnowDeformationManager skeleton
- Configuration settings
- Unit tests (optional)

---

## PHASE 2: Subdivision Cache Management

**Goal**: Manage lifecycle of subdivided terrain chunks

### Tasks

#### 2.1 Create Subdivision Cache Manager

**File**: `components/terrain/subdivisioncache.hpp` + `.cpp`

**Purpose**: Track which chunks are subdivided, when to subdivide/release

```cpp
namespace Terrain {

struct SubdividedChunkData {
    osg::ref_ptr<osg::Geometry> geometry;     // Subdivided mesh
    ChunkKey key;                              // Chunk identifier
    int subdivisionLevel;                      // 0-3
    float maxDeformationRemaining;             // Max deformation in chunk
    float timeSincePlayerLeft;                 // Age tracking
    osg::Vec3f chunkCenter;                    // World position

    bool canRelease() const {
        // Release if deformation negligible OR player left long ago
        return (maxDeformationRemaining < 0.01f) ||
               (timeSincePlayerLeft > 300.0f); // 5 minutes
    }
};

class SubdivisionCache {
public:
    SubdivisionCache(int maxChunks = 50);
    ~SubdivisionCache();

    // Get or create subdivided chunk
    osg::Geometry* getSubdividedChunk(
        const ChunkKey& key,
        const osg::Geometry* original,
        int subdivisionLevel
    );

    // Check if chunk is subdivided
    bool isSubdivided(const ChunkKey& key) const;

    // Update chunk activity (player nearby)
    void markChunkActive(const ChunkKey& key, const osg::Vec3f& playerPos);

    // Update deformation level in chunk (from deformation texture)
    void updateChunkDeformation(const ChunkKey& key, float maxDeformation);

    // Release old chunks (call every frame)
    void update(float dt, const osg::Vec3f& playerPos);

    // Force release all chunks
    void clear();

    // Statistics
    int getNumSubdividedChunks() const;
    size_t getMemoryUsage() const;

private:
    std::map<ChunkKey, SubdividedChunkData> mSubdividedChunks;
    int mMaxChunks;

    // Release oldest/furthest chunks if over limit
    void enforceMemoryLimit(const osg::Vec3f& playerPos);
};

} // namespace Terrain
```

**Implementation**:

```cpp
// subdivisioncache.cpp

#include "subdivisioncache.hpp"
#include "terrainsubdivider.hpp"
#include <components/debug/debuglog.hpp>

namespace Terrain {

SubdivisionCache::SubdivisionCache(int maxChunks)
    : mMaxChunks(maxChunks)
{
    Log(Debug::Info) << "SubdivisionCache created, max chunks: " << maxChunks;
}

SubdivisionCache::~SubdivisionCache() {
    clear();
}

osg::Geometry* SubdivisionCache::getSubdividedChunk(
    const ChunkKey& key,
    const osg::Geometry* original,
    int subdivisionLevel) {

    // Check if already subdivided
    auto it = mSubdividedChunks.find(key);
    if (it != mSubdividedChunks.end()) {
        // Already exists
        return it->second.geometry.get();
    }

    // Enforce memory limit before creating new
    enforceMemoryLimit(osg::Vec3f(0,0,0)); // TODO: pass player pos

    // Subdivide
    Log(Debug::Verbose) << "Subdividing chunk at level " << subdivisionLevel;
    osg::ref_ptr<osg::Geometry> subdivided =
        TerrainSubdivider::subdivide(original, subdivisionLevel);

    if (!subdivided) {
        Log(Debug::Warning) << "Failed to subdivide chunk";
        return nullptr;
    }

    // Cache it
    SubdividedChunkData data;
    data.geometry = subdivided;
    data.key = key;
    data.subdivisionLevel = subdivisionLevel;
    data.maxDeformationRemaining = 0.0f;
    data.timeSincePlayerLeft = 0.0f;
    // data.chunkCenter = ... (set from chunk key)

    mSubdividedChunks[key] = data;

    Log(Debug::Info) << "Subdivided chunk cached. Total: "
                    << mSubdividedChunks.size();

    return subdivided.get();
}

bool SubdivisionCache::isSubdivided(const ChunkKey& key) const {
    return mSubdividedChunks.find(key) != mSubdividedChunks.end();
}

void SubdivisionCache::markChunkActive(
    const ChunkKey& key, const osg::Vec3f& playerPos) {

    auto it = mSubdividedChunks.find(key);
    if (it != mSubdividedChunks.end()) {
        // Reset age timer (player is here)
        it->second.timeSincePlayerLeft = 0.0f;
    }
}

void SubdivisionCache::updateChunkDeformation(
    const ChunkKey& key, float maxDeformation) {

    auto it = mSubdividedChunks.find(key);
    if (it != mSubdividedChunks.end()) {
        it->second.maxDeformationRemaining = maxDeformation;
    }
}

void SubdivisionCache::update(float dt, const osg::Vec3f& playerPos) {
    // Update age timers and release old chunks
    std::vector<ChunkKey> toRelease;

    for (auto& pair : mSubdividedChunks) {
        SubdividedChunkData& data = pair.second;

        // Increase age if player not nearby
        float distSq = (data.chunkCenter - playerPos).length2();
        float releaseDistSq = 1024.0f * 1024.0f; // 1024 units

        if (distSq > releaseDistSq) {
            data.timeSincePlayerLeft += dt;
        }

        // Mark for release if eligible
        if (data.canRelease()) {
            toRelease.push_back(data.key);
        }
    }

    // Release
    for (const auto& key : toRelease) {
        Log(Debug::Verbose) << "Releasing subdivided chunk (decayed)";
        mSubdividedChunks.erase(key);
    }

    if (!toRelease.empty()) {
        Log(Debug::Info) << "Released " << toRelease.size()
                        << " subdivided chunks. Remaining: "
                        << mSubdividedChunks.size();
    }
}

void SubdivisionCache::enforceMemoryLimit(const osg::Vec3f& playerPos) {
    if (mSubdividedChunks.size() <= static_cast<size_t>(mMaxChunks)) {
        return; // Under limit
    }

    // Find furthest chunk from player
    ChunkKey furthestKey;
    float maxDistSq = 0.0f;

    for (const auto& pair : mSubdividedChunks) {
        float distSq = (pair.second.chunkCenter - playerPos).length2();
        if (distSq > maxDistSq) {
            maxDistSq = distSq;
            furthestKey = pair.first;
        }
    }

    // Release furthest
    Log(Debug::Warning) << "Memory limit reached, releasing furthest chunk";
    mSubdividedChunks.erase(furthestKey);
}

void SubdivisionCache::clear() {
    Log(Debug::Info) << "Clearing subdivision cache";
    mSubdividedChunks.clear();
}

int SubdivisionCache::getNumSubdividedChunks() const {
    return mSubdividedChunks.size();
}

size_t SubdivisionCache::getMemoryUsage() const {
    // Rough estimate: 2.3 MB per chunk at level 2
    return mSubdividedChunks.size() * 2.3 * 1024 * 1024;
}

} // namespace Terrain
```

**Success Criteria**:
- Cache stores and retrieves subdivided chunks
- Memory limit enforced
- Old chunks released when conditions met
- No memory leaks

#### 2.2 Integrate Cache with SnowDeformationManager

**File**: `components/terrain/snowdeformation.hpp`

**Add to class**:
```cpp
class SnowDeformationManager {
    // ... existing members ...

private:
    std::unique_ptr<SubdivisionCache> mSubdivisionCache;

public:
    // Get subdivided chunk for rendering
    osg::Geometry* getSubdividedChunk(
        const ChunkKey& key,
        const osg::Geometry* original,
        float distanceToPlayer
    );

    // Check if chunk should be subdivided
    bool shouldSubdivideChunk(
        const ChunkKey& key,
        float distanceToPlayer,
        bool hasSnowTexture
    );
};
```

**Implementation additions**:
```cpp
// In constructor:
SnowDeformationManager::SnowDeformationManager(...) {
    // ...
    int maxChunks = Settings::Manager::getInt(
        "snow max subdivided chunks", "Terrain");
    mSubdivisionCache = std::make_unique<SubdivisionCache>(maxChunks);
}

bool SnowDeformationManager::shouldSubdivideChunk(
    const ChunkKey& key,
    float distanceToPlayer,
    bool hasSnowTexture) {

    if (!hasSnowTexture) return false;
    if (distanceToPlayer > 1024.0f) return false; // Too far

    return true;
}

osg::Geometry* SnowDeformationManager::getSubdividedChunk(
    const ChunkKey& key,
    const osg::Geometry* original,
    float distanceToPlayer) {

    // Determine subdivision level based on distance
    int level = 0;
    if (distanceToPlayer < 256.0f) {
        level = Settings::Manager::getInt(
            "snow subdivision level close", "Terrain");
    } else if (distanceToPlayer < 512.0f) {
        level = Settings::Manager::getInt(
            "snow subdivision level medium", "Terrain");
    } else {
        level = Settings::Manager::getInt(
            "snow subdivision level far", "Terrain");
    }

    // Get or create subdivided chunk
    return mSubdivisionCache->getSubdividedChunk(key, original, level);
}
```

**Success Criteria**:
- Manager uses subdivision cache
- Distance-based subdivision levels work
- Chunks cached and reused appropriately

### Testing Phase 2

**Test Scenario 1**: Cache Lifecycle
- Create multiple subdivided chunks
- Verify caching works (same chunk returned on second request)
- Walk away, verify chunks released after timeout

**Test Scenario 2**: Memory Limit
- Force creation of 100 chunks (exceeding limit of 50)
- Verify oldest chunks released
- Verify no crashes or memory corruption

**Test Scenario 3**: Distance-Based Subdivision
- Walk towards terrain
- Verify subdivision level increases as you approach
- Verify smooth transitions

**Deliverables**:
- SubdivisionCache class
- Integration with SnowDeformationManager
- Distance-based subdivision logic
- Memory management working

---

## PHASE 3: Deformation Texture Generation

**Goal**: Generate deformation data via render-to-texture

*[Rest of phases continue similar to original plan, but now assuming subdivided geometry exists]*

### Tasks

#### 3.1 RTT Camera Setup

**File**: `components/terrain/snowdeformation.cpp`

**Add to SnowDeformationManager**:
```cpp
class SnowDeformationManager {
private:
    osg::ref_ptr<osg::Camera> mRTTCamera;
    osg::ref_ptr<osg::Texture2D> mDeformationTexture[2]; // Ping-pong
    int mCurrentTextureIndex;
    float mWorldTextureSize;
    int mTextureResolution;

    void setupRTTCamera(osg::Group* rootNode);
    void createDeformationTextures();
};
```

**Implementation**:
```cpp
void SnowDeformationManager::setupRTTCamera(osg::Group* rootNode) {
    mTextureResolution = Settings::Manager::getInt(
        "snow deformation resolution", "Terrain");
    mWorldTextureSize = Settings::Manager::getFloat(
        "snow deformation radius", "Terrain");

    // Create RTT camera
    mRTTCamera = new osg::Camera;
    mRTTCamera->setRenderTargetImplementation(
        osg::Camera::FRAME_BUFFER_OBJECT);
    mRTTCamera->setRenderOrder(osg::Camera::PRE_RENDER);

    // Orthographic projection (top-down view)
    mRTTCamera->setProjectionMatrixAsOrtho(
        -mWorldTextureSize, mWorldTextureSize,
        -mWorldTextureSize, mWorldTextureSize,
        -100.0f, 100.0f
    );

    // View from above
    mRTTCamera->setViewMatrixAsLookAt(
        osg::Vec3(0, 100, 0),  // Eye
        osg::Vec3(0, 0, 0),    // Center
        osg::Vec3(0, 0, 1)     // Up (north)
    );

    // Clear to black (no deformation)
    mRTTCamera->setClearMask(GL_COLOR_BUFFER_BIT);
    mRTTCamera->setClearColor(osg::Vec4(0, 0, 0, 0));
    mRTTCamera->setViewport(0, 0, mTextureResolution, mTextureResolution);

    // Attach texture (will swap each frame)
    mRTTCamera->attach(osg::Camera::COLOR_BUFFER, mDeformationTexture[0]);

    // Start disabled
    mRTTCamera->setNodeMask(0);

    rootNode->addChild(mRTTCamera);

    Log(Debug::Info) << "RTT camera created: " << mTextureResolution
                    << "x" << mTextureResolution;
}

void SnowDeformationManager::createDeformationTextures() {
    for (int i = 0; i < 2; ++i) {
        mDeformationTexture[i] = new osg::Texture2D;
        mDeformationTexture[i]->setTextureSize(
            mTextureResolution, mTextureResolution);
        mDeformationTexture[i]->setInternalFormat(GL_RGBA16F);
        mDeformationTexture[i]->setSourceFormat(GL_RGBA);
        mDeformationTexture[i]->setSourceType(GL_FLOAT);
        mDeformationTexture[i]->setFilter(
            osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
        mDeformationTexture[i]->setFilter(
            osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
        mDeformationTexture[i]->setWrap(
            osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mDeformationTexture[i]->setWrap(
            osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    }

    mCurrentTextureIndex = 0;

    Log(Debug::Info) << "Deformation textures created (ping-pong)";
}
```

**Success Criteria**:
- RTT camera created without errors
- Textures created and configured
- Camera renders (even if empty for now)

#### 3.2 Footprint Stamping Shaders

**Files**:
- `files/shaders/compatibility/snow_footprint.vert`
- `files/shaders/compatibility/snow_footprint.frag`

**Vertex Shader**:
```glsl
#version 120

uniform vec2 deformationCenter;  // Player XZ in world space
uniform float deformationRadius; // Coverage radius

varying vec2 stampUV;

void main() {
    // Quad vertices in world XZ space
    vec2 worldXZ = gl_Vertex.xz;

    // Convert to deformation texture UV
    vec2 relativePos = worldXZ - deformationCenter;
    stampUV = (relativePos / deformationRadius) * 0.5 + 0.5;

    // Output in NDC
    gl_Position = vec4(stampUV * 2.0 - 1.0, 0.0, 1.0);
}
```

**Fragment Shader**:
```glsl
#version 120

uniform sampler2D currentDeformation; // Previous frame
uniform vec2 footprintCenter;         // UV space (0.5, 0.5)
uniform float footprintRadius;        // UV space
uniform float deformationDepth;       // World units
uniform float currentTime;            // Game time

varying vec2 stampUV;

void main() {
    // Sample previous deformation
    vec4 prevDeform = texture2D(currentDeformation, stampUV);

    // Distance from footprint center
    float dist = length(stampUV - footprintCenter);

    // Smooth circular falloff
    float influence = smoothstep(footprintRadius, footprintRadius * 0.5, dist);

    // New deformation (additive, keep maximum)
    float newDepth = max(prevDeform.r, influence * deformationDepth);

    // Reset age where new footprint applied
    float age = (influence > 0.01) ? currentTime : prevDeform.g;

    gl_FragData[0] = vec4(newDepth, age, 0.0, 1.0);
}
```

**Success Criteria**:
- Shaders compile
- Render circular footprint to texture
- Previous footprints preserved (ping-pong working)

*[Continue with remaining phases similar to original plan...]*

---

## File Structure Summary

### New Files to Create

```
Subdivision System:
components/terrain/terrainsubdivider.hpp
components/terrain/terrainsubdivider.cpp
components/terrain/subdivisioncache.hpp
components/terrain/subdivisioncache.cpp
apps/openmw_test_subdivision/main.cpp (test program)

Snow Detection:
components/terrain/snowdetection.hpp
components/terrain/snowdetection.cpp

Deformation Manager:
components/terrain/snowdeformation.hpp
components/terrain/snowdeformation.cpp

Shaders:
files/shaders/compatibility/snow_footprint.vert
files/shaders/compatibility/snow_footprint.frag
files/shaders/compatibility/snow_decay.vert
files/shaders/compatibility/snow_decay.frag

Configuration:
[Modifications to files/settings-default.cfg]

CMake:
[Add new files to components/terrain/CMakeLists.txt]
```

### Modified Files

```
Core Terrain:
components/terrain/material.cpp           (snow detection, shader defines)
components/terrain/terraindrawable.cpp    (use subdivided geometry, bind uniforms)
components/terrain/chunkmanager.cpp       (check for subdivision)
components/terrain/world.cpp              (integrate manager)
files/shaders/compatibility/terrain.vert  (sample deformation, displace vertices)

Rendering:
apps/openmw/mwrender/renderingmanager.cpp (create manager, update)

Settings:
files/settings-default.cfg                (all snow deformation settings)

Build:
components/terrain/CMakeLists.txt         (add new source files)
```

---

## Revised Timeline Estimate

### Conservative Estimate (with testing)

- **Phase 0**: 8-12 hours (subdivision system, critical foundation)
- **Phase 1**: 6-8 hours (texture detection, manager skeleton)
- **Phase 2**: 8-10 hours (subdivision cache management)
- **Phase 3**: 12-16 hours (RTT, footprint stamping)
- **Phase 4**: 16-20 hours (terrain shader integration, most complex)
- **Phase 5**: 6-8 hours (decay system)
- **Phase 6**: 10-14 hours (optimization, polish)

**Total for MVP**: 66-88 hours (increased due to subdivision complexity)

---

## Success Criteria (Overall - Updated)

### Minimum Viable Product (MVP)
- [ ] Triangle subdivision system working
- [ ] Texture-based snow detection working
- [ ] Footprints appear on any snow texture (Vvardenfell, Solstheim, mods)
- [ ] Footprints persist for configurable time
- [ ] Footprints visible and smooth (not blocky)
- [ ] Footprints gradually fade (decay)
- [ ] System can be enabled/disabled
- [ ] No crashes or major bugs
- [ ] 60 FPS maintained on reference hardware
- [ ] Memory usage under 250 MB for deformation system

---

## Next Steps - REVISED

1. **Start with Phase 0**: Implement and test subdivision system FIRST
2. **Validate subdivision quality**: Ensure it produces acceptable visual results
3. **Phase 1**: Texture-based detection
4. **Phase 2**: Subdivision cache
5. **Phase 3**: RTT and footprint stamping
6. **Phase 4**: Terrain shader integration (using subdivided geometry)
7. **Incremental testing**: Test thoroughly after each phase
8. **Iterative refinement**: Adjust parameters based on visual results

---

## Document Version

- **Version**: 2.0 (REVISED)
- **Date**: 2025-11-15
- **Changes from v1.0**:
  - Added Phase 0: Mesh Subdivision Foundation
  - Changed from cell-based to texture-based snow detection
  - Added SubdivisionCache management system
  - Revised memory estimates and performance targets
  - Reordered phases to address vertex density first
- **Status**: Ready for Implementation
- **Next Review**: After Phase 0 completion

---

## End of Revised Plan
