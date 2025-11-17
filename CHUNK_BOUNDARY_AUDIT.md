# Chunk Boundary Seam Audit

## Problem Description

Vertices at chunk boundaries have different heights even though they're at the same world position. One vertex might be at height 0 (rock) and the adjacent one at height 50 (snow), creating visible gaps.

## Code Audit - UV Calculation Issue

### Location: `components/terrain/terrainweights.cpp:114-120`

The code attempts to use world-coordinate-based UV sampling, but **it's still chunk-relative**!

```cpp
// Step 3: Calculate the chunk's coverage area in cell coordinates
osg::Vec2f chunkOrigin = chunkCenter - osg::Vec2f(chunkSize * 0.5f, chunkSize * 0.5f);

// Step 4: Calculate UV as position within the chunk's area
float u = (cellPos.x() - chunkOrigin.x()) / chunkSize;
float v = (cellPos.y() - chunkOrigin.y()) / chunkSize;
```

### Concrete Example: Boundary Vertex Between Two Chunks

**Assumptions:**
- Chunk size: 0.125 cells (1024 world units for Morrowind)
- Cell world size: 8192 units
- Boundary vertex world position: (512, 0, 0)

**Chunk A:**
- Center: (0.0, 0.0) in cell units = (0, 0) world units
- Chunk origin: (-0.0625, -0.0625)
- Chunk coverage: [-0.0625 to +0.0625] in cell units

**Chunk B:**
- Center: (0.125, 0.0) in cell units = (1024, 0) world units
- Chunk origin: (0.0625, -0.0625)
- Chunk coverage: [+0.0625 to +0.1875] in cell units

**Boundary vertex at world position (512, 0):**

#### In Chunk A's calculation:
```
vertexPos = (512, 0, 0) in chunk-local coords
worldPos2D = (0.0 * 8192 + 512, 0.0 * 8192 + 0) = (512, 0)
cellPos = (512 / 8192, 0 / 8192) = (0.0625, 0.0)
chunkOrigin = (0.0, 0.0) - (0.0625, 0.0625) = (-0.0625, -0.0625)
u = (0.0625 - (-0.0625)) / 0.125 = 0.125 / 0.125 = 1.0
v = (0.0 - (-0.0625)) / 0.125 = 0.0625 / 0.125 = 0.5
UV = (1.0, 0.5)
```

→ **Samples from the RIGHT EDGE (u=1.0) of Chunk A's blendmap**

#### In Chunk B's calculation:
```
vertexPos = (-512, 0, 0) in chunk-local coords
worldPos2D = (0.125 * 8192 + (-512), 0.0 * 8192 + 0) = (512, 0)  [SAME world pos!]
cellPos = (512 / 8192, 0 / 8192) = (0.0625, 0.0)  [SAME cell pos!]
chunkOrigin = (0.125, 0.0) - (0.0625, 0.0625) = (0.0625, -0.0625)
u = (0.0625 - 0.0625) / 0.125 = 0.0 / 0.125 = 0.0
v = (0.0 - (-0.0625)) / 0.125 = 0.0625 / 0.125 = 0.5
UV = (0.0, 0.5)
```

→ **Samples from the LEFT EDGE (u=0.0) of Chunk B's blendmap**

## The Core Problem

Even though the comment at line 117 claims:
> "This is still chunk-relative, BUT now it's based on consistent world coords
> So the same world position always produces the same UV"

**This is FALSE!** The UV coordinates are different:
- Chunk A: UV = (1.0, 0.5)
- Chunk B: UV = (0.0, 0.5)

The same world position produces **different UVs** because the calculation is relative to each chunk's origin (`chunkOrigin`), which is different for each chunk!

## Why Different UVs Cause Gaps

Each chunk has its own separate blendmap texture (generated in `components/esmterrain/storage.cpp:482`).

When sampling:
- Chunk A: `sampleBlendmap(chunkA_blendmap, (1.0, 0.5))`
  → Samples pixel at `x = (1.0 * (blendmapWidth - 1)) = last pixel`

- Chunk B: `sampleBlendmap(chunkB_blendmap, (0.0, 0.5))`
  → Samples pixel at `x = (0.0 * (blendmapWidth - 1)) = 0 = first pixel`

If these pixels represent different terrain types:
- Chunk A edge pixel = snow texture → weight = (1, 0, 0, 0)
- Chunk B edge pixel = rock texture → weight = (0, 0, 0, 1)

Then vertex heights become:
- Chunk A vertex: lift = 1.0 * 100 (snow) = 100 units
- Chunk B vertex: lift = 0.0 * 100 (snow) = 0 units

**Gap height = 100 units!** (or 50 if one is deformed)

## Why Blendmaps Don't Match at Edges

The blendmaps are generated independently for each chunk from the underlying land data. Even though they should theoretically match at boundaries, mismatches occur due to:

1. **Different sampling regions**: Each chunk samples a different rectangular region of the land data
2. **Edge pixel differences**: The land texture data may have discontinuities at cell boundaries
3. **Interpolation/rounding**: Converting from land texture indices to blendmap pixels can introduce errors
4. **Blendmap generation logic**: The code at `storage.cpp:516` uses `sampleBlendmaps()` which may have edge cases

## Proposed Solutions

### Solution 1: Global UV Coordinates (Incomplete Implementation)

The INTENDED fix is to use world-coordinate-based UVs, but it's implemented incorrectly. The UV should be calculated relative to a GLOBAL reference frame, not each chunk's origin.

**Current (wrong):**
```cpp
osg::Vec2f chunkOrigin = chunkCenter - osg::Vec2f(chunkSize * 0.5f, chunkSize * 0.5f);
float u = (cellPos.x() - chunkOrigin.x()) / chunkSize;  // ← chunk-relative!
```

**Corrected approach would need:**
```cpp
// Don't use chunk-specific blendmaps at all!
// Instead, sample directly from land data using cellPos
```

### Solution 3: Direct Land Data Sampling

Instead of sampling from per-chunk blendmap textures, sample directly from the underlying land texture data using world/cell coordinates. This would bypass the blendmap texture issue entirely.

Need to check if there's a function like:
```cpp
Storage::getTextureAt(cellPos) → texture name → classify → weight
```

## Next Steps

1. ✅ Identify the bug in UV calculation (DONE - it's chunk-relative, not world-absolute)
2. Check if there's a way to sample land data directly using world coordinates
3. Implement a fix that ensures the same world position always samples the same underlying terrain data
