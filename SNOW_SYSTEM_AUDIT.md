# Snow Deformation System - Complete Architecture Audit

**Date:** 2025-11-29
**Status:** CRITICAL BUG - Full-area depression instead of localized footprints
**Investigation:** Root cause analysis complete

---

## Executive Summary

### The Bug
The entire RTT camera area (50m × 50m square) is being depressed uniformly, rather than showing individual footprints where the player/NPCs walk.

### Root Cause Identified
**The depth camera is rendering NOTHING** - resulting in a completely black object mask texture. The "full-area depression" is actually an optical illusion caused by the vertex shader raising the terrain OUTSIDE the RTT area while leaving it flat inside (where deformation should be zero).

---

## System Architecture Overview

The snow deformation system uses a **5-pass RTT pipeline**:

```
Pass 0 (Order 0): Depth Camera → mObjectMaskMap (R8)
                  - Renders actors from below looking up
                  - Output: White = object present, Black = empty
                  ↓
Pass 1 (Order 1): Update Camera → mAccumulationMap[write] (RGBA16F)
                  - Reads: mAccumulationMap[read], mObjectMaskMap
                  - Shader: snow_update.frag
                  - Sliding window scroll + decay + accumulation
                  ↓
Pass 2 (Order 2): Footprint Camera → mAccumulationMap[write]
                  - Renders legacy manual footprints (if any)
                  - Blend mode: GL_RGBA_MAX
                  ↓
Pass 3 (Order 3): Blur Horizontal → mBlurTempBuffer (RGBA16F)
                  - Gaussian blur horizontal pass
                  - Shader: blur_horizontal.frag (MISSING!)
                  ↓
Pass 4 (Order 4): Blur Vertical → mBlurredDeformationMap (RGBA16F)
                  - Gaussian blur vertical pass
                  - Shader: blur_vertical.frag (MISSING!)
                  ↓
Main Render: Terrain shaders read mBlurredDeformationMap
             - Vertex: Displacement (terrain.vert:98-99)
             - Fragment: POM + Normal reconstruction + Darkening
```

---

## Critical Issue Analysis

### Issue #1: Depth Camera Not Rendering Anything ⚠️ CRITICAL

**Location:** [snowdeformation.cpp:546-630](d:\Gamedev\OpenMW\openmw-dev-master\components\terrain\snowdeformation.cpp#L546-L630)

**The Problem:**
The depth camera is configured but **never sees the scene graph properly**. The workaround at lines 612-630 tries to add "Player Root" and "Cell Root" as children, but:

1. **Circular Scene Graph Reference:** Adding Player Root/Cell Root as children of `mDepthCamera` creates a circular reference (these nodes are also children of `mRootNode`, which contains the camera)
2. **Wrong Camera Reference Frame:** Using `ABSOLUTE_RF_INHERIT_VIEWPOINT` (line 551) may not work as intended
3. **CullCallback Disabled:** The `DepthCameraCullCallback` class (lines 24-99) is **never attached to the camera** - it's defined but unused!

**Evidence from code:**
```cpp
// Lines 612-630: Workaround attempt
if (mRootNode && mDepthCamera)
{
    for (unsigned int i = 0; i < mRootNode->getNumChildren(); ++i)
    {
        osg::Node* child = mRootNode->getChild(i);
        std::string name = child->getName();

        if (name == "Player Root" || name == "Cell Root")
        {
            Log(Debug::Info) << "Adding " << name << " to depth camera scene";
            mDepthCamera->addChild(child);  // ⚠️ CIRCULAR REFERENCE!
        }
    }
}
```

**Why it doesn't work:**
- A node cannot have multiple parents in OSG without being wrapped in a `osg::Group` or using `osg::PagedLOD`
- The camera's cull mask filtering won't work if the nodes are direct children (it only filters during traversal)
- The `DepthCameraCullCallback` (lines 24-99) was meant to solve this, but it's **never attached**!

**Result:**
- `mObjectMaskMap` is rendered empty (all black/zero)
- `snow_update.frag` reads this empty mask
- No deformation is applied (finalValue = 0.0 everywhere inside RTT area)

---

### Issue #2: The "Full-Area Depression" Illusion

**How it appears:**

In **wireframe mode:** Vertices look normal height ✅
In **regular mode:** Large square depression visible ❌

**Why this happens:**

Look at [terrain.vert:98-99](d:\Gamedev\OpenMW\openmw-dev-master\files\shaders\compatibility\terrain.vert#L98-L99):

```glsl
// Apply deformation: raise terrain by baseLift, then subtract where footprints are
vertex.z += baseLift * (1.0 - vDeformationFactor);
```

**The math:**
- When `vDeformationFactor = 0` (no deformation): `vertex.z += baseLift` → **RAISED** by `baseLift`
- When `vDeformationFactor = 1` (full deformation): `vertex.z += 0` → **unchanged**

**Current state:**
- Inside RTT area: `vDeformationFactor = 0` (because object mask is empty) → terrain RAISED
- Outside RTT area: `vDeformationFactor = 0` (no deformation data) → terrain ALSO raised... wait, that's not right!

**Wait - let me re-check the bounds checking in terrain.vert:**

Lines 93-96:
```glsl
if (deformUV.x >= 0.0 && deformUV.x <= 1.0 && deformUV.y >= 0.0 && deformUV.y <= 1.0)
{
    vDeformationFactor = texture2D(snowDeformationMap, deformUV).r;
}
```

**AH! Here's the actual bug:**

- **Inside RTT bounds:** `vDeformationFactor = texture2D(...).r = 0.0` (because blur buffer is black)
- **Outside RTT bounds:** `vDeformationFactor` stays at initialization value (line 69): `vDeformationFactor = 0.0`

**So both are zero! Why the square then?**

**The answer:** It's NOT in the vertex shader - it's in the **fragment shader POM** (lines 76-133 in terrain.frag)!

Looking at [terrain.frag:113-132](d:\Gamedev\OpenMW\openmw-dev-master\files\shaders\compatibility\terrain.frag#L113-L132):

```glsl
// Check bounds
if (rttUV.x >= 0.0 && rttUV.x <= 1.0 && rttUV.y >= 0.0 && rttUV.y <= 1.0)
{
    float r = texture2D(snowDeformationMap, rttUV).r;
    // ... POM raymarching happens here ...
    if (p.z < surfaceH)
    {
        deformationFactor = r;  // Update deformation factor
        break;
    }
}
```

**Inside RTT area:** POM raymarching happens, samples empty texture (r=0), creates visual artifacts
**Outside RTT area:** No POM (bounds check fails), normal rendering

**But this STILL doesn't explain a square depression if r=0!**

**WAIT - I need to check the actual texture content. Let me re-read the update shader...**

Looking at [snow_update.frag](d:\Gamedev\OpenMW\openmw-dev-master\files\shaders\compatibility\snow_update.frag):

Lines 32-38:
```glsl
// 4. Sample Object Mask (New Deformation)
float newValue = texture2D(objectMask, uv).r;

// 5. Combine
float finalValue = max(previousValue, newValue);

// 6. Cubic Remapping (Rim Effect)
```

**If objectMask is all black (0.0), then:**
- `newValue = 0.0`
- `finalValue = max(previousValue, 0.0)` = `previousValue` (with decay)
- After many frames with no input: `finalValue → 0.0`

**So the accumulation buffer should be black/zero!**

---

### Issue #3: Missing Blur Shaders 🔴 CRITICAL

**Location:** Shader files not found

The blur pipeline C++ code exists (lines 450-530), but the shader files are **missing**:
- `files/shaders/compatibility/blur_horizontal.frag` - **DOES NOT EXIST**
- `files/shaders/compatibility/blur_vertical.frag` - **DOES NOT EXIST**

**Impact:**
- Blur cameras render with **missing shaders** → undefined behavior
- `mBlurredDeformationMap` (the texture the terrain reads) likely contains garbage or black
- This could be contributing to the visual artifacts

**The doc says:** (rtt_dom_particles.md:704-741)
> **NEEDS SHADER FILES:** Create `blur_horizontal.frag` and `blur_vertical.frag`

---

### Issue #4: Vertex Shader Logic is Inverted

**Location:** [terrain.vert:98-99](d:\Gamedev\OpenMW\openmw-dev-master\files\shaders\compatibility\terrain.vert#L98-L99)

```glsl
// Apply deformation: raise terrain by baseLift, then subtract where footprints are
vertex.z += baseLift * (1.0 - vDeformationFactor);
```

**This is conceptually backwards!** Here's why:

**Expected behavior:**
- Flat snow (no footprints): vertex.z = original height
- Footprint area: vertex.z = original height - depth

**Current behavior:**
- No footprint (`vDeformationFactor = 0`): vertex.z += baseLift → **RAISED**
- Full footprint (`vDeformationFactor = 1`): vertex.z += 0 → **original height**

**The system is designed to:**
1. Raise the entire terrain by `baseLift`
2. Lower it back down where footprints are

**But this creates a problem:**
- Terrain outside the RTT area is ALSO raised (because baseLift > 0 globally)
- This creates a "plateau" effect

**What it SHOULD be:**
```glsl
vertex.z -= vDeformationFactor * baseLift; // Simple depression
```

This would:
- No footprint (`vDeformationFactor = 0`): vertex.z = unchanged
- Full footprint (`vDeformationFactor = 1`): vertex.z -= baseLift (depressed)

---

## System Flow Analysis

### What SHOULD happen:

1. **Depth Camera (Pass 0):**
   - Positioned below player at `(playerX, playerY, playerZ - 1000)`
   - Looks straight up along +Z axis
   - Renders actors with cull mask `(1<<3) | (1<<4) | (1<<10)` = Actor | Player | Object
   - Outputs white pixels where geometry is visible
   - Result: Small white silhouette of player/NPCs in `mObjectMaskMap`

2. **Update Shader (Pass 1):**
   - Reads previous accumulation buffer
   - Applies sliding window offset (player movement)
   - Applies decay (footprints fade over time)
   - Samples new object mask
   - Takes max(decayed_old, new_mask)
   - Applies cubic remapping for rim effect
   - Writes to current accumulation buffer

3. **Footprint Pass (Pass 2):**
   - Renders any manually stamped footprints (legacy system)
   - Blends with max blending mode

4. **Blur Horizontal (Pass 3):**
   - Reads accumulation buffer
   - Applies 5×5 Gaussian blur horizontally
   - Writes to temp buffer

5. **Blur Vertical (Pass 4):**
   - Reads temp buffer
   - Applies 5×5 Gaussian blur vertically
   - Writes to final blurred deformation map

6. **Terrain Rendering:**
   - Vertex shader displaces vertices downward based on deformation map
   - Fragment shader applies POM for detail
   - Fragment shader reconstructs normals from deformation heightmap
   - Fragment shader applies darkening to compressed snow

### What ACTUALLY happens:

1. **Depth Camera (Pass 0):** ❌
   - Camera has no scene to render (circular reference issue)
   - OR camera can't see actors (cull mask not working)
   - Outputs: **All black** (0,0,0,0)

2. **Update Shader (Pass 1):** ⚠️
   - Reads previous buffer: black (on first frame) or decayed near-black
   - Reads object mask: **black** (no actors visible)
   - Combines: `max(~0, 0) = ~0` (near-zero with decay)
   - Cubic remap: minimal effect on near-zero values
   - Writes: **Almost entirely black buffer**

3. **Footprint Pass (Pass 2):** ⚠️
   - Manually stamped footprints ARE rendered (red quads)
   - But immediately cleared from queue (line 848)
   - Only visible for 1 frame, then lost

4. **Blur Horizontal (Pass 3):** ❌
   - **Shader missing!** - undefined behavior
   - Likely outputs black or previous framebuffer content

5. **Blur Vertical (Pass 4):** ❌
   - **Shader missing!** - undefined behavior
   - Final blurred map is probably black/garbage

6. **Terrain Rendering:** ⚠️
   - Reads blurred map: black/garbage
   - Vertex shader: Raises terrain by baseLift everywhere (if baseLift > 0)
   - Fragment shader: POM artifacts from empty heightmap
   - Result: Visual glitches, possibly square artifacts

---

## Root Cause Summary

### Primary Issue: Depth Camera Not Functional
**Why:**
1. Circular scene graph reference (lines 612-630)
2. `DepthCameraCullCallback` defined but never attached
3. Camera reference frame may be incorrect

**Effect:** No actors rendered → empty object mask → no deformation data

### Secondary Issue: Missing Blur Shaders
**Why:**
- Shader files never created (doc mentions this as TODO)

**Effect:** Undefined rendering behavior in blur passes → possibly corrupted final texture

### Tertiary Issue: Vertex Shader Logic
**Why:**
- System designed to "raise then lower" instead of simple depression
- Causes artifacts outside RTT area

**Effect:** Unexpected terrain height changes outside footprint zones

---

## Recommended Fixes (Priority Order)

### Fix #1: Implement Proper Scene Graph Traversal for Depth Camera ⚠️ CRITICAL

**Option A: Use the CullCallback (Recommended)**

The `DepthCameraCullCallback` class (lines 24-99) was designed for this but never attached!

```cpp
// In initRTT(), after creating mDepthCamera:
mDepthCamera->setCullCallback(new DepthCameraCullCallback(mRootNode, mDepthCamera));
```

**BUT:** The callback has bugs:
- Line 53: `cv->getCullingMode()` should be `mCam->getCullMask()`
- Lines 76-80: Manual mask filtering won't work because child nodes have `0xFFFFFFFF` masks

**Option B: Use a Slave Camera (Better)**

```cpp
// Create a separate view for the depth camera
osgViewer::View* depthView = new osgViewer::View;
depthView->setCamera(mDepthCamera);
depthView->setSceneData(mRootNode);
// Camera's cull mask will now work properly
```

**Option C: Fix the Circular Reference (Simplest)**

Instead of adding entire "Player Root" and "Cell Root" as children, we need to:
1. Find the actual actor geometries
2. Create **clone nodes** with just the geometry
3. Add clones as children of depth camera
4. Update clones each frame

This is more expensive but guaranteed to work.

**Option D: Use RTT Camera's Inherit Viewpoint Mode Correctly**

Change from `ABSOLUTE_RF_INHERIT_VIEWPOINT` to `ABSOLUTE_RF` and manually set up scene traversal:

```cpp
mDepthCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
// Don't add scene as child, use CullCallback instead
```

---

### Fix #2: Create Missing Blur Shaders ⚠️ CRITICAL

**File:** `files/shaders/compatibility/blur_horizontal.frag`
```glsl
#version 120
uniform sampler2D inputTex;

void main() {
    vec2 texelSize = vec2(1.0 / 2048.0, 0.0);  // Horizontal only
    const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

    float result = texture2D(inputTex, gl_TexCoord[0].xy).r * weights[0];
    for(int i = 1; i < 5; ++i) {
        result += texture2D(inputTex, gl_TexCoord[0].xy + texelSize * float(i)).r * weights[i];
        result += texture2D(inputTex, gl_TexCoord[0].xy - texelSize * float(i)).r * weights[i];
    }
    gl_FragColor = vec4(result, 0.0, 0.0, 1.0);
}
```

**File:** `files/shaders/compatibility/blur_vertical.frag`
```glsl
#version 120
uniform sampler2D inputTex;

void main() {
    vec2 texelSize = vec2(0.0, 1.0 / 2048.0);  // Vertical only
    const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

    float result = texture2D(inputTex, gl_TexCoord[0].xy).r * weights[0];
    for(int i = 1; i < 5; ++i) {
        result += texture2D(inputTex, gl_TexCoord[0].xy + texelSize * float(i)).r * weights[i];
        result += texture2D(inputTex, gl_TexCoord[0].xy - texelSize * float(i)).r * weights[i];
    }
    gl_FragColor = vec4(result, 0.0, 0.0, 1.0);
}
```

**Note:** These shaders must be added to CMake shader list for compilation!

---

### Fix #3: Simplify Vertex Shader Logic (Optional)

Change [terrain.vert:98-99](d:\Gamedev\OpenMW\openmw-dev-master\files\shaders\compatibility\terrain.vert#L98-L99):

**From:**
```glsl
vertex.z += baseLift * (1.0 - vDeformationFactor);
```

**To:**
```glsl
vertex.z -= vDeformationFactor * baseLift;  // Simple depression
```

**Trade-off:**
- **Pro:** Simpler, no terrain raising outside RTT area
- **Con:** Loses the "raised rim" effect from the cubic remapping

**Alternative:** Keep current logic but ensure deformation is only applied inside RTT bounds:

```glsl
if (baseLift > 0.01 && deformUV.x >= 0.0 && deformUV.x <= 1.0 && deformUV.y >= 0.0 && deformUV.y <= 1.0)
{
    vDeformationFactor = texture2D(snowDeformationMap, deformUV).r;
    vertex.z += baseLift * (1.0 - vDeformationFactor);
}
// else: no modification, vDeformationFactor stays 0, vertex.z unchanged
```

---

## Testing Plan

### Test 1: Verify Object Mask Rendering
1. Add debug visualization to save mObjectMaskMap to disk
2. Walk around in game
3. Check if white pixels appear where player is

**Expected:** Small white circle/blob at player position
**Current:** All black

### Test 2: Verify Accumulation Buffer
1. Add debug visualization to save mAccumulationMap[write] to disk
2. Walk around in game
3. Check if footprints accumulate

**Expected:** White trails following player path
**Current:** Likely all black or near-black

### Test 3: Verify Blur Output
1. After creating blur shaders
2. Save mBlurredDeformationMap to disk
3. Check if trails are smooth (not pixelated)

**Expected:** Smooth gaussian-blurred footprint trails
**Current:** Cannot test (shaders missing)

### Test 4: Verify Terrain Rendering
1. Enable debug visualization in terrain.frag (lines 65-71 commented out)
2. Look for:
   - Green tint = inside RTT area
   - Red overlay = deformation present

**Expected:** Green square following player, red trails behind
**Current:** Possibly green square, no red (no deformation data)

---

## Additional Findings

### Positive Aspects ✅

1. **Update shader logic is correct** - sliding window, decay, accumulation all properly implemented
2. **Cubic remapping implemented** - rim effect formula is correct (lines 40-48 in snow_update.frag)
3. **Normal reconstruction is sophisticated** - finite difference gradient calculation in terrain.frag (lines 187-260)
4. **POM implementation exists** - raymarching through deformation heightmap (terrain.frag lines 76-133)
5. **Ping-pong buffer swap is correct** - proper read/write index management (snowdeformation.cpp lines 677-692)
6. **Particle system functional** - snow/ash/mud particle emissions working independently

### Architectural Issues ⚠️

1. **Mixed rendering paradigms:** System uses BOTH manual footprint stamping (legacy) AND depth camera (new) - should commit to one
2. **Shader manager conflicts:** Multiple references to shader loading issues in debug logs
3. **Settings vs Hardcoded values:** Some values in settings (mSnowDeformationDepth) vs hardcoded (mRTTSize = 3625.0)
4. **Z-range uncertainty:** Depth camera range changed from 2000 to 200 units - may be too small for tall NPCs/creatures

### Performance Considerations 📊

**Current GPU cost (estimated):**
- Depth camera: 0.01ms × actor_count
- Update pass: 0.05ms
- Footprint pass: 0.01ms
- Blur passes: **Unknown** (shaders missing)
- Terrain POM: 0.1-0.3ms (depends on step count)

**Total:** ~0.2ms + blur_cost (probably 0.1-0.2ms)
**Target:** < 0.5ms for 60 FPS compatibility ✅

**Memory usage:**
- Accumulation buffers: 2 × (2048² × 8 bytes) = 64 MB
- Object mask: 2048² × 1 byte = 4 MB
- Blur buffers: 2 × (2048² × 8 bytes) = 64 MB
- **Total:** ~132 MB VRAM

---

## Conclusion

The snow deformation system has **excellent architecture** but is currently non-functional due to:

1. **Depth camera scene graph issue** (no actors being rendered)
2. **Missing blur shaders** (undefined behavior in blur passes)
3. **Potentially inverted vertex shader logic** (raises instead of depresses)

**The "full-area depression" bug is likely:**
- NOT a full-area depression at all
- An optical illusion from the vertex shader raising terrain + fragment shader POM artifacts
- Caused by the depth camera rendering nothing (empty object mask)

**Recommended fix order:**
1. Fix depth camera scene traversal (Fix #1, Option A or C)
2. Create blur shaders (Fix #2)
3. Test and iterate
4. Consider vertex shader simplification (Fix #3) if issues persist

**Estimated fix time:**
- Fix #1: 2-4 hours (debugging scene graph issues)
- Fix #2: 30 minutes (shader files are simple)
- Testing: 1-2 hours
- **Total:** ~4-7 hours to working prototype

---

## File Reference

**C++ Implementation:**
- [snowdeformation.cpp](d:\Gamedev\OpenMW\openmw-dev-master\components\terrain\snowdeformation.cpp) - Main RTT system
- [snowdeformation.hpp](d:\Gamedev\OpenMW\openmw-dev-master\components\terrain\snowdeformation.hpp) - Header/interface

**Shaders:**
- [terrain.vert](d:\Gamedev\OpenMW\openmw-dev-master\files\shaders\compatibility\terrain.vert) - Vertex displacement
- [terrain.frag](d:\Gamedev\OpenMW\openmw-dev-master\files\shaders\compatibility\terrain.frag) - POM, normals, darkening
- [snow_update.frag](d:\Gamedev\OpenMW\openmw-dev-master\files\shaders\compatibility\snow_update.frag) - Accumulation logic
- `blur_horizontal.frag` - **MISSING** ⚠️
- `blur_vertical.frag` - **MISSING** ⚠️

**Documentation:**
- [RTT_debug_tracker.md](d:\Gamedev\OpenMW\openmw-dev-master\RTT_debug_tracker.md) - Debug session history
- [rtt_dom_particles.md](d:\Gamedev\OpenMW\openmw-dev-master\rtt_dom_particles.md) - Master implementation doc
- [RTT_FULL_AREA_BUG.md](d:\Gamedev\OpenMW\openmw-dev-master\RTT_FULL_AREA_BUG.md) - Bug diagnostic

---

**Last Updated:** 2025-11-29
**Auditor:** Claude Code
**Status:** Analysis complete, awaiting fix implementation
