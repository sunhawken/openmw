# Hybrid Snow Deformation & Particles - Master Implementation Document

## Overview
We are implementing a **Hybrid Snow Deformation System** based on the academic paper "Real-time Snow Deformation" (Hanák, 2021), which replicates the technique used in *Horizon Zero Dawn: The Frozen Wilds*. This system combines multiple rendering technologies to achieve realistic, deep snow effects that work in open-world environments.

### Core Technologies
1.  **RTT (Render-To-Texture)**: Generates a persistent, dynamic heightmap of deformation around the player using ping-pong buffers
2.  **Depth Projection**: Automatically captures all dynamic objects (NPCs, creatures, player) by rendering from below
3.  **Vertex Deformation**: Physically lowers terrain vertices to create the footprint silhouette
4.  **Visual Enhancement (POM/Darkening/Normals)**: Fragment shader techniques for high-frequency detail without geometric cost
5.  **Particles**: Snow/ash/mud spray effects on footsteps for added realism

### Key Design Principles
- **Scalability**: Works in infinite open worlds via sliding window
- **Persistence**: Footprints remain until decayed, not frame-limited
- **Performance**: Constant GPU cost regardless of trail length
- **Automation**: All actors deform snow automatically without per-object setup

---

## Coordinate System (CRITICAL)

**OpenMW/Morrowind uses Z-UP coordinate system:**
- **Ground Plane**: XY (horizontal)
- **Vertical Axis**: Z (altitude)
- **Camera conventions**:
  - Looking down from above: Eye at +Z, looking toward -Z
  - Looking up from below: Eye at -Z, looking toward +Z

This affects all camera setup, normal calculations, and shader math. The paper uses the same convention.

---

## Research & References

### Primary Reference: Academic Paper
**"Real-time Snow Deformation" (Master's Thesis, Daniel Hanák, 2021)**
- Based on *Horizon Zero Dawn: The Frozen Wilds* technique
- Full implementation details with shader code
- Performance benchmarks on multiple GPUs
- File: `realtimepaper.md`

**Key Sections:**
- **Section 4.1**: Rendering Orthographic Height (Depth Projection)
- **Section 4.2**: Deformation Heightmap (Ping-Pong, Blur, Normal Reconstruction)
- **Section 4.3**: Sliding Window (Open World Support)
- **Section 4.4-4.5**: Vertex Shader & Tessellation
- **Section 4.6**: Pixel Shader (PBR, Material Blending)

### Secondary Reference: God of War
**[God of War Snow Rendering](https://mamoniem.com/behind-the-pretty-frames-god-of-war/#6-3snow-mesh)**
- Uses RTT for trail data (confirmed approach)
- Heavy use of POM for visual depth (geometry stays simple)
- Hybrid vertex displacement + fragment shader detail

---

## Implementation Status

### ✅ 1. Particle System (COMPLETE)
**Files:**
- `components/terrain/snowparticleemitter.hpp`
- `components/terrain/snowparticleemitter.cpp`

**Technology:**
- Uses OSG's `osgParticle` system
- `RadialShooter` for spray effects
- Triggered from `SnowDeformationManager::stampFootprint`

**Features:**
- Multi-terrain support: Snow (white), Ash (grey), Mud (brown)
- Configurable particle count, speed, lifetime
- Position-based emission synchronized with footprint stamping

---

### ✅ 2. RTT System Architecture (COMPLETE - 3-Pass Pipeline)

**Files:**
- `components/terrain/snowdeformation.hpp`
- `components/terrain/snowdeformation.cpp`

#### Pass 0: Depth Projection (Object Mask)
**Camera:** `mDepthCamera` (Render Order: PRE_RENDER, 0)
**Target:** `mObjectMaskMap` (GL_R8, 2048×2048)
**Purpose:** Capture all dynamic objects touching the ground

**Setup (Z-Up Corrected):**
```cpp
// Camera positioned BELOW ground, looking UP
Eye:    (0, 0, -10000)  // Below terrain
Center: (0, 0, 0)
Up:     (0, -1, 0)      // Flipped Y for UV alignment (intentional)

// Orthographic projection covering RTT area
Projection: Ortho(
    playerX - halfSize, playerX + halfSize,
    playerY + halfSize, playerY - halfSize,  // Top/Bottom swapped (Y-flip compensation)
    0.0, 20000.0  // Z-range
)
```

**Cull Mask:** `(1 << 3) | (1 << 4) | (1 << 10)` = Actor | Player | Object
**Output:** Binary mask (White = object present, Black = empty)
**Shader Override:** Simple white output for all rendered geometry

**Notes:**
- Up vector `(0, -1, 0)` and swapped projection bounds work together to ensure UV alignment with RTT camera
- Renders low-LOD meshes for performance (similar to shadow casting LODs)

#### Pass 1: Update & Accumulation
**Camera:** `mUpdateCamera` (Render Order: PRE_RENDER, 1)
**Target:** `mAccumulationMap[writeIndex]` (GL_RGBA16F, 2048×2048)
**Inputs:**
- `previousFrame` (Texture Unit 0): `mAccumulationMap[readIndex]`
- `objectMask` (Texture Unit 1): `mObjectMaskMap`

**Purpose:** Sliding window scroll, decay, and merge new deformation

**Shader:** `files/shaders/compatibility/snow_update.frag`
**Algorithm:**
1. Calculate `oldUV = currentUV + offset` (sliding window)
2. Sample previous frame at `oldUV` (with bounds check)
3. Apply decay: `previousValue = max(0, previousValue - decayAmount)`
4. Sample new object mask at `currentUV`
5. Merge: `finalValue = max(previousValue, newObjectValue)`
6. Write to current accumulation buffer

**Uniforms:**
- `offset` (vec2): UV space offset from player movement
- `decayAmount` (float): Per-frame decay (dt / decayTime)

**Ping-Pong Logic:**
- Read from `mAccumulationMap[readIndex]`
- Write to `mAccumulationMap[writeIndex]`
- Swap indices each frame

#### Pass 2: Legacy Footprint Rendering (Transitional)
**Camera:** `mRTTCamera` (Render Order: PRE_RENDER, 2)
**Target:** `mAccumulationMap[writeIndex]` (same as Pass 1)
**Purpose:** Render any manually-stamped footprints (array-based fallback)

**Setup:**
```cpp
// Camera positioned ABOVE ground, looking DOWN
Eye:    (0, 0, 10000)
Center: (0, 0, 0)
Up:     (0, 1, 0)

Projection: Ortho(
    playerX - halfSize, playerX + halfSize,
    playerY - halfSize, playerY + halfSize,
    0.0, 20000.0
)
```

**Rendering:**
- Draws quads for each footprint in `mFootprints` deque
- Uses `GL_RGBA_MAX` blend mode (max blending)
- Clears `mFootprints` after rendering (single-use in persistent mode)

**Note:** This pass may be removed once full depth projection handles all deformation

---

### ✅ 3. Terrain Shaders (COMPLETE - Normal Reconstruction Working)

#### Vertex Shader: `terrain.vert`
**Functionality:**
- Samples `snowDeformationMap` at world position
- Calculates UV: `deformUV = (worldPos.xy - snowRTTWorldOrigin.xy) / snowRTTScale + 0.5`
- Displaces vertex downward: `vertexPos.z -= deformationValue * maxDepth`
- Passes `vDeformationFactor`, `passWorldPos`, `vMaxDepth` to fragment shader

**Uniforms:**
- `snowDeformationMap` (sampler2D, Unit 7)
- `snowRTTWorldOrigin` (vec3)
- `snowRTTScale` (float)

#### Fragment Shader: `terrain.frag`
**Features Implemented:**

1. **Normal Reconstruction** (Lines 203-276) ✅
   - **Method:** Finite difference gradient calculation
   - **Algorithm:**
     ```glsl
     float texelSize = 1.0 / 2048.0;
     float h = texture2D(snowDeformationMap, deformUV).r;
     float h_r = texture2D(snowDeformationMap, deformUV + vec2(texelSize, 0)).r;
     float h_u = texture2D(snowDeformationMap, deformUV + vec2(0, texelSize)).r;

     float stepWorld = snowRTTScale * texelSize;
     float dX = (h - h_r) * vMaxDepth / stepWorld;  // Slope in X
     float dY = (h - h_u) * vMaxDepth / stepWorld;  // Slope in Y

     vec3 worldPerturb = normalize(vec3(-dX, -dY, 1.0)); // Z-up normal
     vec3 viewPerturb = normalize(gl_NormalMatrix * worldPerturb);

     vec3 viewUp = normalize(gl_NormalMatrix * vec3(0, 0, 1.0));
     vec3 diff = viewPerturb - viewUp;
     viewNormal = normalize(viewNormal + diff);
     ```
   - **Result:** Footprint edges catch light correctly, self-shadowing works

2. **Parallax Occlusion Mapping (POM)** (Lines 98-148) ✅
   - **Method:** Raymarching through deformation heightmap
   - **Steps:** 10 steps, adaptive step size based on `vMaxDepth`
   - **Algorithm:**
     - Start ray slightly behind fragment position
     - March along view direction
     - Check if ray Z < surface height at each step
     - Update `deformationFactor` when intersection found
   - **Status:** Implemented for correct depth perception

3. **Darkening** (Line 152) ✅
   - Simple diffuse darkening: `diffuseColor.rgb *= (1.0 - deformationFactor * 0.4)`
   - Simulates compressed/wet snow appearance

**Debug Visualization** (Lines 65-87):
- Green tint: Inside RTT area
- Red overlay: Deformation present (intensity = deformation amount)
- Useful for verifying RTT alignment and coverage

---

## Critical Analysis vs. Academic Paper

### ✅ What We Implemented Correctly

| Feature | Paper | Our Implementation | Status |
|---------|-------|-------------------|--------|
| **Ping-Pong Buffers** | Section 4.2, line 655 | `mAccumulationMap[2]` with swap | ✅ CORRECT |
| **Depth Projection** | Section 4.1 | `mDepthCamera` from below | ✅ CORRECT |
| **Sliding Window** | Section 4.3 | UV offset in update shader | ✅ CORRECT |
| **Normal Reconstruction** | Section 4.2, lines 689-754 | Finite difference in frag shader | ✅ CORRECT |
| **Persistent Accumulation** | Core technique | Ping-pong + max blending | ✅ CORRECT |
| **Z-Up Coordinate System** | Figure 4.1 | All cameras/normals use Z-up | ✅ CORRECT |

### ⚠️ Critical Missing Features

#### 1. **Gaussian Blur Filter** - MISSING (CRITICAL)
**Paper Reference:** Section 4.2, lines 674-688

**What the paper does:**
```
Deformation → Cubic Remap → Blur Horizontal → Blur Vertical → Normal Computation
```

**Current pipeline:**
```
Deformation → (nothing) → Normal Computation  ❌
```

**Why it matters:**
- **Sharp edges:** Footprints have hard, pixelated edges without blur
- **Lighting artifacts:** Non-smooth normals create jagged specular highlights
- **Unrealistic appearance:** Real snow has smooth transitions, not binary holes

**Paper implementation:**
- Separable 5×5 Gaussian kernel
- Two passes: horizontal then vertical
- Uses LDS (compute shader) to cache texels and reduce memory bandwidth
- Stores filtered result in intermediate buffer before normal computation

**What we need:**
Two additional cameras in the pipeline:
```
Pass 1: Update (current)
Pass 2: Blur Horizontal → TempBuffer
Pass 3: Blur Vertical → FinalBuffer (read by terrain)
Pass 4: Footprint render (current)
```

**Shader pseudo-code:**
```glsl
// blur_horizontal.frag
uniform sampler2D inputTex;
const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main() {
    vec2 texelSize = 1.0 / textureSize(inputTex, 0);
    float result = texture(inputTex, uv).r * weights[0];
    for(int i = 1; i < 5; ++i) {
        result += texture(inputTex, uv + vec2(texelSize.x * i, 0.0)).r * weights[i];
        result += texture(inputTex, uv - vec2(texelSize.x * i, 0.0)).r * weights[i];
    }
    gl_FragColor = vec4(result, 0, 0, 1);
}
```

**Priority:** CRITICAL - This is not optional, it's core to the technique's visual quality

---

#### 2. **Cubic Remapping (Edge Elevation)** - MISSING (HIGH)
**Paper Reference:** Section 4.2, Equation 4.1, lines 658-665

**The Formula:**
```
f(x) = -3.4766x³ + 6.508x² - 2.231x + 0.215
```

**What it does:**
- Creates the "rim" effect around footprints (snow piles up at edges)
- Depresses undeformed snow slightly (x=0 → f(0)=0.215 instead of 0)
- Applied AFTER accumulation, BEFORE blur

**Why it matters:**
- **Realism:** Real footprints have raised edges where snow is pushed aside
- **Visual pop:** The rim catches light and creates the iconic footprint look
- **Depth perception:** Helps distinguish deep vs. shallow deformation

**Current approach:**
We rely on footprint texture alpha gradients, which doesn't create actual geometric rim

**What we need:**
Add to update shader OR as separate pass:
```glsl
// In snow_update.frag, after line 38:
float remap(float x) {
    return -3.4766 * x*x*x + 6.508 * x*x - 2.231 * x + 0.215;
}

float finalValue = max(previousValue, newValue);
finalValue = remap(finalValue);  // Apply cubic function
gl_FragColor = vec4(finalValue, 0, 0, 1);
```

**Note:** Paper uses polynomial regression to fit this curve to desired visual result

**Priority:** HIGH - Significantly improves visual quality, relatively easy to add

---

#### 3. **Terrain Heightmap Integration** - MISSING (MEDIUM)
**Paper Reference:** Section 4.2, lines 641-649

**The Problem:**
Our depth camera renders from a fixed plane (Z = -10000). This works for:
- ✅ Flat terrain (Solstheim snow fields)
- ❌ Hills/mountains (floating objects create footprints, embedded objects don't)

**What the paper does:**
```glsl
// In update shader:
float terrainHeight = texture2D(terrainHeightmap, uv).r;
float objectDepth = texture2D(depthBuffer, uv).r;
float snowSurfaceHeight = terrainHeight + snowThickness;

if (objectDepth > terrainHeight && objectDepth < snowSurfaceHeight) {
    // Object is in the snow layer, apply deformation
    deformation = (snowSurfaceHeight - objectDepth) / snowThickness;
}
```

**What we need:**
1. Access to OpenMW's terrain heightmap texture
2. Modify update shader to check terrain height
3. Convert depth camera's depth values to world Z coordinates

**Current workaround:**
Works acceptably on flat terrain; may have issues on slopes

**Priority:** MEDIUM - Important for varied terrain, but not blocking for initial implementation

---

#### 4. **Actual Depth Values** - CONSIDERATION (LOW)
**Paper Reference:** Section 4.1, line 640

**Paper uses:** 16-bit linear depth buffer
**We use:** 8-bit binary mask (GL_R8)

**Trade-off:**
- **Paper:** Stores actual depth → can distinguish foot from torso → more accurate
- **Ours:** Binary presence → simpler, faster, works if we only render low-LOD meshes

**When it matters:**
- Tall objects (NPC torso/head) might create unwanted deformation
- Overlapping objects lose depth information

**Mitigation:**
- Only render feet/legs to depth camera (custom cull mask or LOD)
- Use low-poly shadow-casting LODs (paper's approach)

**Priority:** LOW - Monitor for artifacts, upgrade if needed

---

### 🔄 Architectural Differences (Acceptable)

#### Tessellation vs. Vertex Displacement
**Paper:** Uses hardware tessellation (Hull/Domain shaders) for smooth geometry
**Ours:** Uses vertex displacement + POM

**Why we differ:**
- OpenMW terrain system may not support dynamic tessellation easily
- POM provides similar visual result without pipeline changes
- Vertex displacement handles the silhouette, POM handles the detail

**Trade-off:**
- ❌ Lower geometric resolution (depends on terrain mesh density)
- ✅ Simpler integration with existing renderer
- ✅ POM provides high-frequency detail cheaply

**Verdict:** Acceptable trade-off for initial implementation

#### Compute Shaders vs. Fragment Shaders
**Paper:** Uses compute shaders with LDS for blur/update passes
**Ours:** Uses fragment shaders with ping-pong buffers

**Why we differ:**
- OSG-based pipeline may not have compute shader infrastructure
- Fragment shader RTT is well-supported and battle-tested

**Trade-off:**
- ❌ Slightly less efficient (no LDS caching)
- ✅ Simpler, more portable
- ✅ Same visual result

**Verdict:** Acceptable trade-off

---

## Remaining Tasks (Prioritized)

### 🔴 CRITICAL (Blocks Visual Quality)
- [ ] **Implement Gaussian Blur**
  - [ ] Create horizontal blur shader
  - [ ] Create vertical blur shader
  - [ ] Add intermediate texture buffers
  - [ ] Insert blur passes into pipeline (between Update and Terrain sampling)
  - [ ] Tune blur kernel size (start with 5×5, paper uses 5×5)

### 🟡 HIGH (Significantly Improves Quality)
- [ ] **Implement Cubic Remapping**
  - [ ] Add remap function to update shader
  - [ ] OR create separate remap pass
  - [ ] Tune coefficients if needed (paper's values are a good start)
  - [ ] Verify edge elevation appears visually

### 🟢 MEDIUM (Correctness on Varied Terrain)
- [ ] **Terrain Height Integration**
  - [ ] Expose OpenMW terrain heightmap to snow system
  - [ ] Modify update shader to check terrain height
  - [ ] Test on hills/mountains (Solstheim has some slopes)

### 🔵 LOW (Polish & Optimization)
- [ ] **Tuning**
  - [ ] Adjust decay time (currently based on settings)
  - [ ] Tune RTT resolution (2048×2048 vs. paper's 1024×1024)
  - [ ] Adjust RTT coverage area (currently ~50m, paper uses 64m)
  - [ ] Fine-tune darkening intensity (currently 0.4)
  - [ ] Optimize particle count/lifetime per terrain type

- [ ] **POM Refinement**
  - [ ] Implement UV shifting (texture parallax)
  - [ ] Currently only using deformation factor for darkening
  - [ ] Would enhance depth illusion further

- [ ] **Debug Features**
  - [ ] Remove/disable debug visualization in terrain.frag (lines 65-87)
  - [ ] Add runtime toggles for blur/remap/normals
  - [ ] Performance profiling per pass

- [ ] **Depth Camera Verification**
  - [ ] Verify UV alignment between depth camera and RTT camera
  - [ ] The flipped up-vector `(0, -1, 0)` is intentional for UV matching
  - [ ] Swapped projection bounds compensate for Y-flip
  - [ ] Confirm visually that object mask aligns with terrain

---

## Technical Details

### Memory Footprint
**Paper (1024×1024, 64m area):** 9 MB VRAM
**Ours (2048×2048, 50m area):**
- Ping-pong buffers: 2 × (2048×2048 × 8 bytes) = 64 MB (RGBA16F)
- Object mask: 2048×2048 × 1 byte = 4 MB (R8)
- **Total:** ~68 MB

**Optimization opportunity:** Match paper's 1024×1024 if memory is concern

### Performance Expectations
**Paper benchmarks (1024×1024):**
- GTX 1070: Deformation 0.05ms, Blur 0.08ms, Normals 0.10ms = **0.23ms total**
- RTX 2070: **0.14ms total**

**Our current (without blur):**
- Update pass: ~0.05ms (estimated)
- Depth pass: 0.01ms × object count
- Footprint pass: negligible (usually 0-1 footprints)

**With blur added:**
- Expect ~0.3-0.5ms total on modern GPU (acceptable for 60 FPS)

### Texture Formats
| Texture | Format | Size | Purpose |
|---------|--------|------|---------|
| `mAccumulationMap[2]` | GL_RGBA16F | 2048² | Ping-pong deformation storage (R=depth) |
| `mObjectMaskMap` | GL_R8 | 2048² | Binary object presence mask |
| TempBlurBuffer (needed) | GL_R16F | 2048² | Intermediate blur result |

**Note:** We only use R channel, but RGBA16F allows future expansion (G=velocity, B=material?)

### Shader Uniform Reference

**Terrain Shaders Receive:**
- `snowDeformationMap` (sampler2D, Unit 7): Final blurred deformation
- `snowRTTWorldOrigin` (vec3): Center of RTT area in world space
- `snowRTTScale` (float): Size of RTT area (world units)

**Update Shader Uses:**
- `previousFrame` (sampler2D, Unit 0): Read buffer from ping-pong
- `objectMask` (sampler2D, Unit 1): Depth camera output
- `offset` (vec2): Sliding window UV offset
- `decayAmount` (float): Per-frame decay

**Legacy Uniforms (Array-based, may be deprecated):**
- `snowFootprintPositions` (vec3[]): Manual footprint positions
- `snowFootprintCount` (int): Number of active footprints
- `snowFootprintRadius` (float): Footprint size
- Various depth uniforms per terrain type

---

## Pipeline Diagram

### Current Pipeline (3 Passes)
```
┌─────────────────────────────────────────────────────────────┐
│ Pass 0: Depth Camera (PRE_RENDER, Order 0)                  │
│ ├─ Render: Actors (low-LOD) from BELOW                      │
│ ├─ View: Eye(0,0,-10000), Up(0,-1,0)                        │
│ └─ Output: mObjectMaskMap (R8, white=object)                │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ Pass 1: Update Camera (PRE_RENDER, Order 1)                 │
│ ├─ Read: mAccumulationMap[read], mObjectMaskMap             │
│ ├─ Shader: snow_update.frag                                 │
│ ├─ Algorithm: Scroll + Decay + Max(old, new)                │
│ └─ Output: mAccumulationMap[write]                          │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ Pass 2: Footprint Camera (PRE_RENDER, Order 2)              │
│ ├─ Render: Manual footprint quads (transitional)            │
│ ├─ Blend: GL_RGBA_MAX                                       │
│ └─ Output: mAccumulationMap[write] (additive)               │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ Main Render: Terrain                                         │
│ ├─ Vertex: Displace using mAccumulationMap[write]           │
│ ├─ Fragment: Normals + POM + Darkening                      │
│ └─ Read: snowDeformationMap (Unit 7)                        │
└─────────────────────────────────────────────────────────────┘
```

### Needed Pipeline (5 Passes - With Blur)
```
Pass 0: Depth Camera → mObjectMaskMap
          ↓
Pass 1: Update Camera → mAccumulationMap[write]
          ↓
Pass 2: Blur Horizontal → mTempBlurBuffer        ← NEW
          ↓
Pass 3: Blur Vertical → mBlurredDeformationMap   ← NEW
          ↓
Pass 4: Footprint Camera → mBlurredDeformationMap (additive)
          ↓
Main Render: Terrain reads mBlurredDeformationMap
```

**Key change:** Terrain reads from blurred buffer, not raw accumulation

---

## Code File Reference

### Core Implementation
- **Manager:** `components/terrain/snowdeformation.cpp` + `.hpp`
  - RTT camera setup (lines 259-437)
  - Update logic (lines 439-634)
  - Ping-pong swap (lines 477-509)

- **Terrain Integration:** `components/terrain/snowdeformationupdater.cpp`
  - Binds texture to terrain StateSet
  - Updates uniforms each frame

### Shaders
- **Update:** `files/shaders/compatibility/snow_update.frag` + `.vert`
  - Sliding window, decay, merge
  - **NEEDS:** Cubic remap addition

- **Terrain Vertex:** `files/shaders/compatibility/terrain.vert`
  - Vertex displacement
  - UV calculation for deformation map sampling

- **Terrain Fragment:** `files/shaders/compatibility/terrain.frag`
  - Normal reconstruction (lines 203-276)
  - POM raymarching (lines 98-148)
  - Darkening (line 152)
  - **NEEDS:** Read from blurred texture instead of raw

### Particles
- **Emitter:** `components/terrain/snowparticleemitter.cpp` + `.hpp`
  - Multi-terrain particle configs
  - Emission on footprint stamp

### Detection
- **Snow Detection:** `components/terrain/snowdetection.cpp` + `.hpp`
  - Texture-based terrain type detection
  - Determines if system should activate

---

## Debugging & Verification

### Visual Debug (Currently Active)
**In terrain.frag (lines 65-87):**
- Green tint = Inside RTT coverage area
- Red overlay = Deformation present (intensity = depth)

**To verify correct operation:**
1. Walk in snow → Should see red footprints
2. RTT area follows player (green box moves)
3. Footprints persist until decay
4. Normal lighting changes at footprint edges

### Common Issues

**No footprints visible:**
- Check texture binding (Unit 7)
- Verify depth camera cull mask matches actor masks
- Check RTT camera render order (PRE_RENDER)
- Verify uniforms reach shaders (snowRTTWorldOrigin, snowRTTScale)

**Footprints in wrong location:**
- UV calculation mismatch
- Depth camera/RTT camera not aligned
- Sliding window offset incorrect

**Footprints don't move with player:**
- Sliding window not updating
- Offset uniform not being set
- Ping-pong swap not happening

**Jagged/pixelated footprints:**
- Expected WITHOUT blur (this is the current state)
- Will be fixed by Gaussian blur implementation

**No lighting on footprint edges:**
- Normal reconstruction not working
- Check gradient calculation in fragment shader
- Verify vMaxDepth is being passed from vertex shader

---

## Next Steps for Implementation

### Phase 1: Gaussian Blur (CRITICAL)
1. Create two new texture buffers:
   - `mBlurTempBuffer` (R16F, 2048×2048)
   - `mBlurredDeformationMap` (R16F, 2048×2048) - replaces direct read of accumulation map

2. Create two new cameras:
   - `mBlurHorizontalCamera` (Order 2)
   - `mBlurVerticalCamera` (Order 3)

3. Write blur shaders:
   - `blur_horizontal.frag`: 5-tap Gaussian in X direction
   - `blur_vertical.frag`: 5-tap Gaussian in Y direction

4. Update terrain to read from `mBlurredDeformationMap` instead of `mAccumulationMap[writeIndex]`

5. Adjust footprint camera to Order 4 (after blur)

### Phase 2: Cubic Remapping (HIGH)
1. Add remap function to `snow_update.frag`
2. Apply after line 38 (after max blending)
3. Test visual appearance of rim effect
4. Optionally tune coefficients

### Phase 3: Terrain Height (MEDIUM)
1. Research OpenMW terrain heightmap access
2. Expose as texture to update shader
3. Modify update logic to check terrain height
4. Test on varied terrain (Solstheim slopes, Vvardenfell mountains)

### Phase 4: Polish (LOW)
1. Remove debug visualization
2. Performance profiling
3. Settings tuning
4. UV parallax in POM

---

## Settings Reference

**Current Settings (from `settings-default.cfg`):**
- `mSnowDeformationEnabled`: Enable/disable system
- `mSnowFootprintRadius`: Footprint size
- `mSnowDeformationDepth`: Max depth of deformation
- `mSnowDecayTime`: Time for footprints to fade (seconds)
- `mSnowMaxFootprints`: Legacy array size (may be deprecated)
- `mAsh*`, `mMud*`: Similar settings for ash/mud terrain

**RTT Constants (in `snowdeformation.cpp`):**
- `mRTTSize`: 3625.0 units (~50 meters in Morrowind scale)
- Texture resolution: 2048×2048
- Texel size: ~0.024 meters (2.4 cm) - slightly finer than paper's 6.25 cm

---

## Conclusion

**Current State:**
- ✅ Core architecture matches paper (ping-pong, depth projection, sliding window)
- ✅ Normal reconstruction working correctly
- ✅ POM implemented for visual depth
- ✅ Particles functional
- ❌ Missing Gaussian blur (critical for quality)
- ❌ Missing cubic remapping (important for realism)
- ❌ Missing terrain height integration (important for hills)

**Visual Quality:**
- Currently: Functional but pixelated/harsh edges
- With blur: Will match paper quality
- With blur + remap: Will match or exceed paper

**Performance:**
- Expected: < 0.5ms on modern GPU (acceptable for 60 FPS)
- Scalable: Constant cost regardless of trail length

**Path Forward:**
Focus on Gaussian blur implementation as the highest priority blocker to visual quality. The other features are important but the system is fundamentally functional without them.
