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

## Distance Thresholds

**Chunk size:** 256 world units

**Subdivision zones** (with 128-unit pre-subdivision buffer):

```cpp
Level 3: distance < 384 + 128 = 512 units  // 3x3 grid
Level 2: distance < 768 + 128 = 896 units  // 5x5 grid
Level 0: distance >= 896 units             // No subdivision
```

**Why buffer?** Ensures chunks are subdivided **before** player reaches them, preventing pop-in.

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

**Weight interpolation:** When triangles subdivide, weights are automatically interpolated at midpoints for smooth terrain transitions.

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
| Subdivision calculation | `chunkmanager.cpp` | 73-90 |
| Trail tracking | `subdivisiontracker.cpp` | 23-175 |
| Distance thresholds | `subdivisiontracker.cpp` | 109-130 |
| Decay logic | `subdivisiontracker.cpp` | 66-107 |

---

## Expected Behavior

✅ **Subdivision pattern always centered on player**
✅ **Smooth updates** (no sudden "all chunks pop")
✅ **Visible trail behind player** (60-second fade)
✅ **No FPS drops** from cache clearing
✅ **Gradual terrain weight transitions** at boundaries
