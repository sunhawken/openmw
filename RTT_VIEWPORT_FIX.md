# RTT Viewport Fix - CRITICAL ISSUE FOUND (IN PROGRESS)

**Date**: 2025-11-16
**Status**: 🟡 **ROOT CAUSE IDENTIFIED - PARTIAL FIX (viewport still being overridden)**
**Issue**: RTT camera viewport being changed during render, PROTECTED flag not working

---

## 🎯 The Problem - FOUND!

After implementing comprehensive diagnostics, the log output revealed the issue:

```
[SNOW DIAGNOSTIC] FBO status: 36053 (COMPLETE)           ✓ FBO is valid
[SNOW DIAGNOSTIC] FBO is COMPLETE - ready to render      ✓ Framebuffer OK
[SNOW DIAGNOSTIC] Viewport: 0,0 1280x720                 ✗ WRONG SIZE!
```

**Expected**: `Viewport: 0,0 1024x1024` (texture resolution)
**Actual**: `Viewport: 0,0 1280x720` (main window resolution)

---

## 🔍 What This Means

The RTT camera was:
- ✅ Being traversed by OSG (cull callbacks executed)
- ✅ Rendering (initial/final draw callbacks executed)
- ✅ Using a valid FBO (status = COMPLETE)
- ✅ Had correct attachments (2 buffers: color + depth)

**BUT:**
- ❌ **Using the wrong viewport!**

When rendering to a 1024x1024 texture with a 1280x720 viewport:
- Geometry coordinates are calculated for the wrong size
- The quad might be positioned outside the texture bounds
- Pixels render to wrong locations or not at all
- Result: Max depth = 0 (nothing renders to texture)

---

## 💡 Why This Happened

In `setupRTT()`, we correctly set:
```cpp
mRTTCamera->setViewport(0, 0, mTextureResolution, mTextureResolution);
// Should be: 0, 0, 1024, 1024
```

**But** when the camera actually renders, the OpenGL viewport is 1280x720.

**Possible causes**:
1. OSG might be overriding the viewport for PRE_RENDER cameras
2. The viewport might be getting reset by the main render loop
3. The viewport setting might not be applied until later
4. OpenMW might have custom viewport handling

---

## ✅ The Fix

### Phase 1: Detection (Already Implemented)

Enhanced `DiagnosticInitialDrawCallback` to:
1. Check the actual GL viewport: `glGetIntegerv(GL_VIEWPORT, glViewport)`
2. Check the camera's intended viewport: `cam->getViewport()`
3. Compare them and log any mismatch

### Phase 2: Automatic Correction (NEW)

If viewport mismatch is detected in the draw callback:
```cpp
if (glViewport[2] != (GLint)vp->width() || glViewport[3] != (GLint)vp->height())
{
    Log(Debug::Error) << "[SNOW DIAGNOSTIC] *** VIEWPORT MISMATCH! ***";
    Log(Debug::Error) << "[SNOW DIAGNOSTIC] GL viewport doesn't match camera viewport!";
    Log(Debug::Error) << "[SNOW DIAGNOSTIC] Forcing viewport to camera settings...";

    // FORCE the correct viewport
    glViewport(vp->x(), vp->y(), vp->width(), vp->height());

    // Verify it stuck
    GLint newViewport[4];
    glGetIntegerv(GL_VIEWPORT, newViewport);
    Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Viewport after force: "
                       << newViewport[2] << "x" << newViewport[3];
}
```

### Phase 3: Verification

Added to `DiagnosticFinalDrawCallback`:
```cpp
GLint viewport[4];
glGetIntegerv(GL_VIEWPORT, viewport);
Log(Debug::Warning) << "[SNOW DIAGNOSTIC] Final viewport: "
                   << viewport[2] << "x" << viewport[3];
```

This confirms the viewport stayed correct during the entire render.

---

## 📊 Expected Log Output (After Fix)

```
[SNOW DIAGNOSTIC] INITIAL Draw Callback #1 - BEFORE RENDER
[SNOW DIAGNOSTIC] FBO status: 36053 (COMPLETE)
[SNOW DIAGNOSTIC] FBO is COMPLETE - ready to render
[SNOW DIAGNOSTIC] GL Viewport: 0,0 1280x720                    ← Wrong!
[SNOW DIAGNOSTIC] Camera's Viewport Setting: 0,0 1024x1024     ← Correct
[SNOW DIAGNOSTIC] *** VIEWPORT MISMATCH! ***
[SNOW DIAGNOSTIC] GL viewport doesn't match camera viewport!
[SNOW DIAGNOSTIC] Forcing viewport to camera settings...
[SNOW DIAGNOSTIC] Viewport after force: 0,0 1024x1024          ← Fixed!
[SNOW DIAGNOSTIC] FINAL Draw Callback #1 - AFTER RENDER
[SNOW DIAGNOSTIC] If you see this, the camera DID execute rendering
[SNOW DIAGNOSTIC] Final viewport: 0,0 1024x1024                ← Stayed correct!

[SNOW DIAGNOSTIC] Max depth byte: 255 (1.0 normalized)         ← SUCCESS!
[SNOW DIAGNOSTIC] Non-zero pixels: 1048576 / 1048576           ← All red!
```

---

## 🎯 What This Should Fix

### Before (Viewport = 1280x720):
- Quad vertices in wrong screen positions
- Geometry clips or misses texture entirely
- Max depth = 0
- Saved images: All black

### After (Viewport = 1024x1024):
- Quad vertices calculated correctly
- Geometry renders to entire texture
- Max depth = 255 (from shader's vec4(1.0, 0, 0, 1))
- Saved images: **Solid red!**

---

## 🧪 Testing Instructions

1. **Rebuild the project** with the latest changes
2. **Run OpenMW** and walk around on snow
3. **Check the log** for:
   - `*** VIEWPORT MISMATCH! ***` - Should appear on first frame
   - `Viewport after force: 0,0 1024x1024` - Confirms fix applied
   - `Final viewport: 0,0 1024x1024` - Confirms it stayed correct
4. **Wait for auto-save** (after 5 footprints)
5. **Check saved image** `snow_footprint_5.png`
   - Should be **SOLID RED** if the fix worked!
   - If still black, we need to investigate further

---

## 📝 Files Modified

### `components/terrain/snowdeformation.cpp`

**Added includes**:
```cpp
#include <osg/Viewport>  // For accessing camera viewport settings
```

**Enhanced `DiagnosticInitialDrawCallback`** (lines 237-272):
- Reports both GL viewport and camera viewport
- Detects mismatch
- Forces correct viewport via `glViewport()`
- Verifies the change stuck

**Enhanced `DiagnosticFinalDrawCallback`** (lines 296-300):
- Reports final viewport after rendering
- Confirms viewport stayed correct throughout render

---

## 🔬 Technical Details

### Why Viewport Matters for RTT

When rendering to texture with orthographic projection:
1. **Vertices** are transformed by projection matrix to NDC (-1 to +1)
2. **Viewport** maps NDC to pixel coordinates
3. **Wrong viewport** = wrong pixel coordinates

**Example with our setup**:
- Orthographic projection: -300 to +300 in X/Y
- Quad vertices: covers entire -300 to +300 range
- Texture: 1024x1024 pixels

**With correct viewport (1024x1024)**:
- Vertex at X=-300 → NDC=-1 → Pixel X=0 ✓
- Vertex at X=+300 → NDC=+1 → Pixel X=1024 ✓

**With wrong viewport (1280x720)**:
- Vertex at X=-300 → NDC=-1 → Pixel X=0 (OK)
- Vertex at X=+300 → NDC=+1 → Pixel X=1280 (OUTSIDE 1024x1024 texture!) ✗
- Also Y dimension is wrong (720 vs 1024)

This causes:
- Quad to be clipped or positioned wrong
- Pixels to render outside texture bounds (discarded)
- Result: Empty texture, max depth = 0

---

## 🚀 Next Steps

### If This Fixes It (Max Depth = 255):

1. **Re-enable clear** (`mRTTCamera->setClearMask(GL_COLOR_BUFFER_BIT)`)
2. **Test with clear enabled** - should work now
3. **Switch to actual footprint shader** (not just solid red)
4. **Test ping-pong blending** with real deformation
5. **Optimize**: Remove diagnostic callbacks (or reduce log frequency)
6. **Consider reverting to GL_RGBA16F** if you prefer float precision

### If This Doesn't Fix It (Still Max Depth = 0):

Then we know:
- ✅ FBO is complete
- ✅ Callbacks execute
- ✅ Viewport is correct
- ❌ But quad still doesn't render

Next diagnostics would be:
1. **Geometry bounds check** - Verify quad isn't being culled
2. **Shader compilation check** - Verify shader compiles without errors
3. **Draw callback on geometry** - Verify quad's draw() is called
4. **Simpler geometry** - Try a single triangle instead of quad
5. **Different Z position** - Try quad at different depths

---

## 📊 Commit History

1. **`567aba40`** - Implemented diagnostic tests (clear disabled, format change, callbacks)
2. **`011940c8`** - Fixed Windows build (OSG GLExtensions usage)
3. **`a604916c`** - **THIS FIX** - Viewport mismatch detection and correction

---

## 🎉 Expected Result

With the viewport fixed, **the quad should render!**

The diagnostic tests (clear disabled, GL_RGBA8 format) are still in effect, so if the viewport fix works:
- ✅ Red pixels will **accumulate** over frames (clear disabled)
- ✅ Max depth will be **255** (full red from shader)
- ✅ Saved images will be **solid red**

This would confirm:
1. ✅ Quad IS rendering
2. ✅ Shader IS running
3. ✅ Texture IS being written
4. ✅ The **ONLY** problem was the viewport!

---

## 🔍 Analysis Summary

**What the diagnostics revealed**:
- Camera traversal: ✅ Working
- FBO completeness: ✅ Working
- Buffer attachments: ✅ Working
- Callbacks executing: ✅ Working
- **Viewport size: ❌ WRONG (1280x720 instead of 1024x1024)**

**What we learned**:
- OSG was overriding or not applying the RTT camera's viewport
- The fix is to force it in the draw callback
- This is a common issue with RTT in some OSG setups

**Confidence level**: 🟢 **HIGH**
- This explains ALL the symptoms
- The fix is targeted and minimal
- Should resolve the max depth = 0 issue

---

**Let's test it!** 🚀

---

## 🔄 UPDATE - Latest Test Results (2025-11-16)

### What We've Implemented:

**Fix Attempt #1**: Force viewport in InitialDrawCallback
- ✅ Successfully detects mismatch (1280x720 vs 1024x1024)
- ✅ Successfully forces to 1024x1024 via `state->applyAttribute(vp)`
- ❌ Viewport changes back to 800x600 during render!

**Fix Attempt #2**: Add viewport to StateSet with PROTECTED flag
```cpp
cameraState->setAttributeAndModes(rttViewport,
    osg::StateAttribute::ON |
    osg::StateAttribute::OVERRIDE |
    osg::StateAttribute::PROTECTED);
```
- ❌ **PROTECTED flag has NO EFFECT**
- Viewport still changes from 1024x1024 → 800x600 during render

### Current Diagnostic Output:

```
[SNOW DIAGNOSTIC] INITIAL Draw Callback #1 - BEFORE RENDER
[SNOW DIAGNOSTIC] GL Viewport: 0,0 1280x720              ← Main window
[SNOW DIAGNOSTIC] Camera's Viewport Setting: 0,0 1024x1024  ← What we want
[SNOW DIAGNOSTIC] *** VIEWPORT MISMATCH! ***
[SNOW DIAGNOSTIC] Forcing viewport via OSG State...
[SNOW DIAGNOSTIC] Viewport after force: 0,0 1024x1024    ✓ Fixed!
[SNOW DIAGNOSTIC] ✓ Viewport correction SUCCEEDED!

[SNOW DIAGNOSTIC] FINAL Draw Callback #1 - AFTER RENDER
[SNOW DIAGNOSTIC] Final viewport: 0,0 800x600            ✗ Changed AGAIN!
```

**Result**:
- Saved images: **Still pitch black** ⬛
- Max depth: **Still 0**
- No terrain deformation visible

---

## 🔍 Key Findings

### What We Know:
1. ✅ **FBO is COMPLETE** (status 36053)
2. ✅ **Callbacks are executing** (cull + initial draw + final draw)
3. ✅ **Camera is rendering** (final callback reached)
4. ✅ **2 buffer attachments** (color + depth)
5. ✅ **Can force viewport to 1024x1024** in initial callback
6. ❌ **Viewport gets overridden to 800x600** during render
7. ❌ **PROTECTED flag doesn't prevent override**

### The Mystery of 800x600:
- **Not** the window size (1280x720)
- **Not** the texture size (1024x1024)
- **Not** a known OpenMW viewport
- Where does `800x600` come from?

### What This Means:
Even though we successfully force the viewport to 1024x1024, something in the OSG or OpenMW rendering pipeline is **changing it back during the actual rendering**. The PROTECTED flag should prevent this, but it doesn't work.

Possible explanations:
1. **OpenMW viewport management** - OpenMW might have custom viewport handling that ignores PROTECTED
2. **OSG render stages** - Some render stage might be resetting viewport
3. **Graphics driver** - Driver might be caching/overriding viewport
4. **Multiple render passes** - Viewport might be correct for one pass but wrong for geometry pass

---

## 🎯 What Needs to Happen Next

The viewport MUST stay at 1024x1024 throughout the entire render. We need to find a way to either:

**Option A**: Find where the viewport is being changed and prevent it
- Add more granular callbacks to track viewport changes
- Check if OpenMW has viewport override mechanisms
- Look for render stage hooks

**Option B**: Force viewport at multiple points during render
- Set viewport in both initial AND final callbacks
- Add viewport setting to the geometry's draw callback
- Override viewport in every possible render stage

**Option C**: Use a different RTT approach entirely
- Use osg::Image as render target instead of texture
- Use FBO directly without OSG Camera abstraction
- Render to texture manually without RTT camera

**Option D**: Force OpenMW to respect our viewport
- Check OpenMW's camera management code
- Look for viewport handling in OpenMW's renderer
- See how other RTT cameras (water, shadows) handle this

---

## 📋 Commit History

1. **`567aba40`** - Diagnostic tests (clear disabled, GL_RGBA8, callbacks)
2. **`011940c8`** - Windows build fix (GLExtensions)
3. **`a604916c`** - Viewport mismatch detection and auto-correction
4. **`fc9d45ea`** - Documentation
5. **`52a6382e`** - Windows build fix (use OSG State for viewport)
6. **`0d299d7b`** - Add viewport to StateSet with PROTECTED flag ← Current

---

## 🚧 Current Status

**Blocker**: Viewport protection is not working. The viewport is being overridden to 800x600 during rendering despite our attempts to protect it.

**Next Steps**: See NEXT_CHALLENGE.md for detailed analysis and proposed solutions.

**Confidence Level**: 🟡 **MEDIUM**
- We've identified the exact problem (viewport override)
- We can detect and temporarily fix it (in initial callback)
- But we can't PREVENT the override (PROTECTED flag fails)
- Need deeper investigation into OSG/OpenMW viewport handling

**Estimated Effort**:
- If there's an OpenMW setting/flag we're missing: **Low** (1-2 hours)
- If we need to patch OpenMW's viewport handling: **Medium** (1 day)
- If we need to switch RTT approaches: **High** (2-3 days)
