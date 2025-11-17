# Chunk Subdivision System

**Purpose:** Dynamic terrain mesh subdivision for high-quality snow/ash/mud deformation that follows the player smoothly.

---

## Core Concept

Terrain chunks are dynamically subdivided based on **distance from player**, creating a grid pattern that always stays centered on the player's position.

---

## Subdivision Levels

| Level | Triangles | Description | Distance from Player |
|-------|-----------|-------------|---------------------|
| **0** | 1x (base) | No subdivision | Beyond 5x5 grid |
| **2** | 16x | Medium detail | Outer ring of 5x5 grid (16 chunks) |
| **3** | 64x | Maximum detail | Inner 3x3 grid (9 chunks) |

**Note:** Level 1 is intentionally skipped - we only use 0, 2, and 3 for simpler transitions.

---

## Grid Pattern (Top-Down View)

```
┌───┬───┬───┬───┬───┐
│ 0 │ 2 │ 2 │ 2 │ 0 │  Legend:
├───┼───┼───┼───┼───┤  0 = No subdivision (default terrain)
│ 2 │ 3 │ 3 │ 3 │ 2 │  2 = Medium (16x triangles)
├───┼───┼───┼───┼───┤  3 = Max (64x triangles)
│ 2 │ 3 │ P │ 3 │ 2 │  P = Player position
├───┼───┼───┼───┼───┤
│ 2 │ 3 │ 3 │ 3 │ 2 │  Grid ALWAYS centered on player
├───┼───┼───┼───┼───┤
│ 0 │ 2 │ 2 │ 2 │ 0 │
└───┴───┴───┴───┴───┘
```

- **3x3 inner grid:** 9 chunks at level 3 (player + 8 adjacent chunks)
- **5x5 total grid:** 25 chunks total, outer 16 at level 2
- **Pattern moves with player** - always centered

---

## Implementation Details

### Chunk Size Filtering

**Critical:** Only **leaf-level terrain chunks** are subdivided, not parent chunks in the quadtree hierarchy.

```cpp
const float MAX_SUBDIVISION_CHUNK_SIZE = 0.125f;  // In cell units

if (size <= MAX_SUBDIVISION_CHUNK_SIZE && mSubdivisionTracker)
{
    subdivisionLevel = mSubdivisionTracker->getSubdivisionLevelFromPlayerGrid(...);
}
```

**Chunk sizes in OpenMW's terrain system:**
- Morrowind (cellSize = 8192): 0.125 cells = **1024 world units**
- ESM4 (cellSize = 4096): 0.125 cells = **512 world units**

**Why this matters:** Without this filter, large parent chunks (1.0, 2.0, 4.0 cells) would be subdivided, causing ALL terrain to subdivide instead of just the 5x5 grid around the player.

### Grid-Based Subdivision (Chebyshev Distance)

Subdivision uses **Chebyshev distance** (max of absolute differences) to create square patterns:

```cpp
// Convert positions to grid coordinates
int playerChunkX = floor(playerWorldPos.x / cellSize);
int playerChunkY = floor(playerWorldPos.y / cellSize);
int chunkGridX = floor(chunkCenter.x);  // Already in cell units
int chunkGridY = floor(chunkCenter.y);

// Calculate grid distance (creates square patterns)
int gridDistance = max(abs(chunkGridX - playerChunkX), abs(chunkGridY - playerChunkY));

// Determine subdivision level
if (gridDistance <= 1)      level = 3;  // 3x3 inner grid
else if (gridDistance <= 2) level = 2;  // 5x5 outer ring
else                        level = 0;  // No subdivision
```

**Grid distance thresholds:**
- **gridDistance ≤ 1:** Level 3 (3x3 grid = 9 chunks)
- **gridDistance ≤ 2:** Level 2 (5x5 grid = 25 chunks total, 16 outer)
- **gridDistance > 2:** Level 0 (no subdivision)

---

## Trail System (Time-Based Decay)

Chunks that player walked through maintain subdivision for a **trail effect**.

### Trail Settings

- **Duration:** 60 seconds
- **Decay start:** 10 seconds
- **Decay method:** Pure time-based (no distance checks)

### Trail Timeline

```
0-10 sec:   Full subdivision (grace period)
10-35 sec:  Level 3 → 2 (first half of decay)
35-60 sec:  Level 2 → 0 (second half of decay)
60+ sec:    Chunk removed from trail tracking
```

**Result:** Player leaves a visible trail of subdivided chunks that gradually fades over 60 seconds.

---

## Cache System

### Cache Key Structure

```cpp
struct ChunkKey {
    osg::Vec2f mCenter;              // Chunk position
    unsigned char mLod;              // Level of detail
    unsigned mLodFlags;              // LOD flags
    unsigned char mSubdivisionLevel; // Subdivision level (NEW)
};
```

**Key insight:** Same chunk at different subdivision levels = different cache entries.

### How It Works

```
Player at (0, 0):
  Chunk A at (256, 0) → distance 256 → level 3
  Cache key: {(256,0), lod=5, flags=0, subdivision=3}
  Cache MISS → create chunk with 64x triangles

Player moves to (100, 0):
  Chunk A at (256, 0) → distance 156 → still level 3
  Cache key: {(256,0), lod=5, flags=0, subdivision=3}  ← Same key
  Cache HIT → return existing chunk ✓ (no recreation)

Player moves to (700, 0):
  Chunk A at (256, 0) → distance 444 → now level 2
  Cache key: {(256,0), lod=5, flags=0, subdivision=2}  ← Different key
  Cache MISS → create chunk with 16x triangles
  Previous level 3 version stays in cache for trail system
```

**No cache clearing on player movement** - subdivision updates smoothly as player crosses distance thresholds.

---

## Integration with Terrain Weights

Each subdivided vertex gets terrain weights for multi-terrain deformation:

```glsl
// Vertex attribute (computed once per chunk)
vec4 terrainWeights;  // (snow, ash, mud, rock)

// Applied in shader
float baseLift = weights.x * snowDepth +     // 100 units
                 weights.y * ashDepth +       // 20 units
                 weights.z * mudDepth;        // 10 units
                 // weights.w (rock) = 0, no deformation
```

### Weight LOD Consistency (Critical Fix)

**Problem:** If weights are computed at different LOD levels for different subdivision levels, terrain will "jump" when chunks change subdivision level.

**Solution:** Force `LOD_FULL` for ALL subdivided chunks regardless of distance:

```cpp
subdivided = TerrainSubdivider::subdivideWithWeights(
    geometry.get(), subdivisionLevel,
    chunkCenter, chunkSize,
    layerList, blendmaps,
    mStorage, mWorldspace,
    mPlayerPosition, cellSize,
    TerrainWeights::LOD_FULL);  // ← Force full LOD for consistency
```

**Why this matters:**
- Ensures weights are computed identically at subdivision level 0, 2, and 3
- Prevents terrain "popping" when chunks change subdivision levels
- Guarantees smooth visual transitions as player moves

**Weight interpolation:** When triangles subdivide, weights are automatically interpolated at midpoints via normalized averaging:

```cpp
osg::Vec4 w_midpoint = normalize((w0 + w1) / 2.0f);
```

This ensures smooth terrain transitions across all boundaries.

---

## Performance Characteristics

### Memory

- **Per chunk:** ~16 bytes/vertex for weights
- **Level 3 chunk:** ~4096 vertices → ~64 KB weights
- **Total (25 chunks):** 9×64KB + 16×16KB = 832 KB (manageable)

### CPU Cost

- **Subdivision:** One-time cost when chunk created
- **Weight computation:** One-time cost when chunk created
- **Per-frame cost:** Only subdivision level lookup (fast)

### Cache Efficiency

- Chunks cached per subdivision level
- No cache thrashing (no clearing on movement)
- Smooth updates (only changed chunks recreated)

---

## Key Design Decisions

1. **Grid-based, not distance-based**
   - Creates predictable 3x3 and 5x5 patterns
   - Matches visual expectations
   - Simpler to reason about

2. **Subdivision in cache key**
   - No sudden updates (no cache clearing)
   - Smooth transitions as player moves
   - Trail system works naturally

3. **Time-only trail decay**
   - Simple, predictable behavior
   - No distance-based complexity
   - Pure 60-second timer

4. **Only 3 levels (0, 2, 3)**
   - Simpler transitions
   - Less cache memory
   - Sufficient quality

---

## Code Locations

| Component | File | Lines |
|-----------|------|-------|
| Cache key definition | `chunkmanager.hpp` | 54-65 |
| Chunk size filtering | `chunkmanager.cpp` | 72-101 |
| Subdivision with LOD forcing | `chunkmanager.cpp` | 467-491 |
| Grid-based calculation | `subdivisiontracker.cpp` | 166-247 |
| Trail tracking update | `subdivisiontracker.cpp` | 23-68 |
| Decay logic | `subdivisiontracker.cpp` | 77-112 |
| Weight interpolation | `terrainweights.cpp` | 212-225 |
| Subdivision with weights | `terrainsubdivider.cpp` | 252-392 |

---

## Troubleshooting

### Issue: All chunks are subdividing, not just around player

**Cause:** Chunk size filter is missing or threshold is too high.

**Solution:** Check `MAX_SUBDIVISION_CHUNK_SIZE` in `chunkmanager.cpp`:
```cpp
const float MAX_SUBDIVISION_CHUNK_SIZE = 0.125f;  // Must be small!
```

**Verification:**
- Add logging: `Log(Debug::Info) << "Chunk size: " << size;`
- You should see mostly small chunks (≤ 0.125) being subdivided
- Large chunks (1.0, 2.0, 4.0) should skip subdivision

### Issue: Terrain "jumps" or "pops" when chunks change subdivision level

**Cause:** Weight LOD is distance-based instead of forced to LOD_FULL.

**Solution:** Check `subdivideWithWeights()` calls in `chunkmanager.cpp`:
```cpp
TerrainWeights::LOD_FULL);  // ← Must be present!
```

**Why:** Distance-based LOD causes weights to differ between subdivision levels, creating visible discontinuities.

### Issue: Subdivision pattern not following player correctly

**Cause:** Player position not being updated, or grid calculation incorrect.

**Verification:**
- Enable debug logging in `subdivisiontracker.cpp` (already present)
- Check log output for `[SUBDIVISION DEBUG]` messages
- Verify `playerChunkX/Y` and `chunkGridX/Y` are correct
- Verify `gridDistance` calculation uses Chebyshev distance (max, not Euclidean)

### Issue: No trail effect, chunks immediately unsubdivide

**Cause:** Trail tracker not being updated, or `mMaxTrailTime` is 0.

**Solution:**
- Ensure `updateSubdivisionTracker(dt)` is called each frame
- Check `mMaxTrailTime` is set to 60.0f in `subdivisiontracker.cpp`
- Verify `markChunkSubdivided()` is called when chunks are created

---

## Expected Behavior

✅ **Subdivision pattern always centered on player**
✅ **Only ~25 chunks subdivided** (9 at level 3, 16 at level 2)
✅ **Smooth updates** (no sudden "all chunks pop")
✅ **No visual "jumping"** when chunks change subdivision level
✅ **Visible trail behind player** (60-second fade)
✅ **No FPS drops** from cache clearing
✅ **Gradual terrain weight transitions** at boundaries

---

## Testing Checklist

- [ ] Walk around and verify only nearby terrain has increased detail
- [ ] Count subdivided chunks (should be ~25 around player)
- [ ] Watch chunks as you move - no sudden popping or jumping
- [ ] Walk across snow/rock boundary - smooth height transition
- [ ] Check verbose logs - grid distances should be 0-2 for subdivided chunks
- [ ] Leave an area and return - trail should still be visible for 60 seconds
- [ ] Check memory usage - should be ~832 KB for 25 chunks (acceptable)

---

## Implementation Notes for Future Developers

### Key Lessons Learned

1. **Chunk Size Matters**
   - OpenMW's terrain uses a quadtree with chunks of varying sizes
   - Must filter by `size <= MAX_SUBDIVISION_CHUNK_SIZE` to only subdivide leaf chunks
   - Without this, parent chunks in the quadtree will also subdivide (wrong!)

2. **Weight LOD Consistency is Critical**
   - ALWAYS force `LOD_FULL` for subdivided chunks
   - Distance-based LOD causes terrain to "jump" when subdivision level changes
   - Small performance cost, but essential for visual quality

3. **Grid-Based vs Distance-Based**
   - Grid-based (Chebyshev distance) creates predictable square patterns
   - Distance-based (Euclidean distance) creates circular patterns (wrong!)
   - Grid-based is simpler to reason about and matches player expectations

4. **Cache Key Includes Subdivision Level**
   - Same chunk at different subdivision levels = different cache entries
   - Enables smooth transitions without cache clearing
   - Trail system works naturally with this design

5. **Weight Interpolation is Automatic**
   - Normalized averaging at midpoints during triangle subdivision
   - No special shader code needed for smooth blending
   - Hardware interpolation handles the rest

### Common Mistakes to Avoid

- ❌ Don't use distance-based LOD for weight computation on subdivided chunks
- ❌ Don't filter chunks by world units - filter by cell units (size parameter)
- ❌ Don't use Euclidean distance for grid calculation - use Chebyshev distance
- ❌ Don't clear the chunk cache on player movement - it's unnecessary
- ❌ Don't skip the chunk size filter - all chunks will subdivide!

### Performance Notes

- Subdivision is CPU-intensive, but only happens once per chunk
- Weight computation adds ~10-20% overhead, but is also one-time
- Per-frame cost is minimal (just grid distance lookup)
- Memory usage is reasonable (~832 KB for 25 chunks)
- Cache efficiency is excellent (no thrashing, no clearing)
