# RTT Camera Diagnostic Tests - Implementation Report

**Date**: 2025-11-16
**Status**: Critical diagnostic tests implemented
**Goal**: Identify why RTT camera max depth = 0

---

## Changes Implemented

### 1. ✅ Disable Clear (HIGH PRIORITY TEST)

**File**: `components/terrain/snowdeformation.cpp:142-157`

**Change**:
```cpp
// BEFORE:
mRTTCamera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

// AFTER (TEST):
mRTTCamera->setClearMask(0);  // No clearing - should accumulate if rendering
```

**Purpose**:
- If the clear operation is overwriting geometry, disabling it will show accumulation
- If quad IS rendering, red pixels should accumulate over multiple frames
- This is the #1 most likely cause according to RTT_NEXT_STEPS.md

**Expected Result**:
- If quad renders: Max depth should be 255 (from shader outputting vec4(1.0, 0, 0, 1))
- If clear was the problem: We'll see red accumulating in saved textures
- If still 0: Problem is elsewhere (quad not rendering OR FBO issue)

---

### 2. ✅ Change Texture Format (MEDIUM PRIORITY TEST)

**File**: `components/terrain/snowdeformation.cpp:239-260`

**Changes**:
```cpp
// BEFORE:
mDeformationTexture[i]->setInternalFormat(GL_RGBA16F_ARB);
mDeformationTexture[i]->setSourceType(GL_FLOAT);

// AFTER (TEST):
mDeformationTexture[i]->setInternalFormat(GL_RGBA8);
mDeformationTexture[i]->setSourceType(GL_UNSIGNED_BYTE);
```

**Purpose**:
- GL_RGBA16F might have driver compatibility issues
- GL_RGBA8 is universally supported
- Test shader outputs 1.0 which maps to 255 in GL_RGBA8

**Expected Result**:
- If float format was the problem: Max depth should be 255
- If still 0: Format wasn't the issue

---

### 3. ✅ Enhanced Diagnostic Callbacks

**File**: `components/terrain/snowdeformation.cpp:176-273`

**Changes**:
1. **Initial Draw Callback** - Executes BEFORE camera renders
   - Checks FBO completeness status
   - Reports viewport settings
   - Logs "INITIAL Draw Callback #N - BEFORE RENDER"

2. **Final Draw Callback** - Executes AFTER camera renders
   - Confirms render completed
   - Logs "FINAL Draw Callback #N - AFTER RENDER"

3. **Enhanced Cull Callback**
   - Reports camera node mask
   - Reports number of children
   - Reports buffer attachments

**Purpose**:
- Verify camera is actually being traversed by OSG
- Verify camera reaches draw phase
- Check FBO completeness in real-time
- Understand execution order (cull → initial draw → render → final draw)

**Expected Output** (if working):
```
[SNOW DIAGNOSTIC] Cull Callback #1
[SNOW DIAGNOSTIC] Camera node mask: 4294967295
[SNOW DIAGNOSTIC] INITIAL Draw Callback #1 - BEFORE RENDER
[SNOW DIAGNOSTIC] FBO status: 36053 (COMPLETE)
[SNOW DIAGNOSTIC] Viewport: 0,0 1024x1024
[SNOW DIAGNOSTIC] FINAL Draw Callback #1 - AFTER RENDER
```

**Diagnostic Interpretation**:
- **No cull callback**: Camera not being traversed (node mask issue?)
- **No initial draw callback**: Camera not reaching draw phase (render order issue?)
- **FBO not COMPLETE**: Framebuffer attachment problem (THIS IS THE ISSUE)
- **All callbacks but max depth = 0**: Quad not rendering OR shader not running

---

### 4. ✅ Updated Texture Readback

**File**: `components/terrain/snowdeformation.cpp:1244-1300`

**Changes**:
- Updated to read GL_UNSIGNED_BYTE instead of GL_FLOAT
- Added max depth byte reporting (0-255 range)
- Added non-zero pixel count
- Improved logging

**Expected Output**:
```
[SNOW DIAGNOSTIC] Max depth byte: 255 (1.0 normalized)
[SNOW DIAGNOSTIC] Non-zero pixels: 1048576 / 1048576  ← All pixels red
```

---

## How to Interpret Results

### Scenario A: Callbacks Execute, Max Depth = 255
**Result**: ✅ **SUCCESS! Quad is rendering!**
- The problem WAS the clear operation OR the float format
- Next step: Re-enable clear with proper timing
- Next step: Implement actual footprint shader

### Scenario B: Callbacks Execute, FBO NOT COMPLETE
**Result**: 🔴 **FBO ATTACHMENT ISSUE**
- The framebuffer is not valid
- Check color buffer attachment (mDeformationTexture)
- Check depth buffer attachment
- Verify texture parameters are valid

### Scenario C: Callbacks Execute, FBO Complete, Max Depth = 0
**Result**: 🟡 **QUAD NOT RENDERING OR SHADER ISSUE**
- Camera is rendering but quad is not visible
- Possible causes:
  - Quad geometry culled (check bounds)
  - Shader not compiling (add compilation checks)
  - Projection/view matrix incorrect (quad outside frustum)
  - Node mask on footprint group wrong

### Scenario D: No Callbacks Execute
**Result**: 🔴 **CAMERA NOT TRAVERSING**
- Camera is not being visited by OSG
- Check PRE_RENDER setup
- Check parent node structure
- Check if camera needs to be added differently

---

## Testing Instructions

1. **Build the project**:
   ```bash
   cd build
   cmake --build . -j$(nproc)
   ```

2. **Run OpenMW and walk around on snow**:
   - The system will stamp footprints automatically
   - After 5, 10, and 20 footprints, textures auto-save

3. **Check logs** (stdout or openmw.log):
   - Look for `[SNOW DIAGNOSTIC]` and `[SNOW RTT TEST]` lines
   - Verify callbacks are executing
   - Check FBO status
   - Check max depth values

4. **Check saved textures**:
   - Files: `snow_footprint_5.png`, `snow_footprint_10.png`, `snow_footprint_20.png`
   - Red pixels = depth value
   - Green pixels = any deformation present
   - Expected: Solid red if working, black if not

---

## Next Steps Based on Results

### If Test Succeeds (Max Depth > 0)
1. Re-enable clear with `mRTTCamera->setClearMask(GL_COLOR_BUFFER_BIT)`
2. Verify clear timing (should clear BEFORE geometry render)
3. Re-test with actual footprint shader (not just solid red)
4. Implement proper ping-pong blending

### If FBO Not Complete
1. Add more detailed FBO attachment checking
2. Verify texture parameters (size, format, type)
3. Try different internal formats (GL_RGBA, GL_RGB8, etc.)
4. Check if depth buffer is causing issues (try without it)

### If Quad Not Rendering
1. Add geometry draw callback to verify quad is being drawn
2. Check quad bounds (might be culled)
3. Add shader compilation error checking
4. Try rendering quad at different Z positions
5. Try using osg::Image render target instead of texture

### If Callbacks Don't Execute
1. Try different camera attachment method
2. Check if PRE_RENDER cameras need special handling in OpenMW
3. Look at other RTT usage in OpenMW (water, shadows)
4. Consider using POST_RENDER instead
5. Try manual render traversal

---

## Files Modified

1. `components/terrain/snowdeformation.cpp`
   - Lines 142-157: Disable clear
   - Lines 247-260: Change texture format to GL_RGBA8
   - Lines 176-273: Enhanced diagnostic callbacks
   - Lines 281-296: Updated setup logging
   - Lines 332-334: Updated texture creation logging
   - Lines 355-356: Updated depth buffer logging
   - Lines 1244-1300: Updated texture readback for GL_RGBA8

---

## Reverting Changes

If you need to revert to previous state:

```bash
# Revert clear:
mRTTCamera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

# Revert texture format:
mDeformationTexture[i]->setInternalFormat(GL_RGBA16F_ARB);
mDeformationTexture[i]->setSourceType(GL_FLOAT);

# Keep diagnostic callbacks (they're useful!)
```

---

## Summary

**What We're Testing**:
1. Is the quad rendering at all? (disable clear to see accumulation)
2. Is float texture format causing issues? (test with GL_RGBA8)
3. Is the camera executing? (verbose callbacks)
4. Is the FBO valid? (completeness check in draw callback)

**Most Likely Issues** (in order):
1. Clear overwriting geometry (TEST #1 will reveal this)
2. FBO not complete (callbacks will show this)
3. Float format incompatibility (TEST #2 will reveal this)
4. Quad not visible to camera (would need geometry-level debugging)

**Expected Timeline**:
- Build: ~5-10 minutes
- Test: ~2 minutes (walk around, wait for auto-save)
- Analysis: Immediate (check logs and images)

---

**Good luck!** 🔍

The diagnostic callbacks will give us definitive answers about WHERE in the rendering pipeline things are failing.
