# RTT Camera Timing Fix - Final Solution

**Date**: 2025-11-16
**Issue**: Quad not rendering to texture (Max depth = 0)
**Root Cause**: Node mask enable/disable timing relative to PRE_RENDER phase

---

## The Problem

The RTT camera was set to PRE_RENDER order, meaning it renders during the draw traversal phase. However, the node mask management was incorrect:

### Original Broken Flow:

**Frame N:**
1. `update()` START: Disables all groups (camera, footprint, blit, decay)
2. `update()` MIDDLE: `stampFootprint()` enables camera + footprint group
3. `update()` END: Returns (groups still enabled)
4. **Draw phase**: PRE_RENDER camera executes ✓

**Frame N+1:**
1. `update()` START: **Disables all groups** ← Camera disabled before drawing!
2. `update()` MIDDLE: Maybe calls `stampFootprint()` or not
3. `update()` END: Returns
4. **Draw phase**: PRE_RENDER tries to execute but camera is DISABLED ✗

### Why This Failed:

The groups were being disabled at the START of the next frame's update, BEFORE that frame's draw phase could execute. This meant:
- Frame N: stampFootprint() enables → Draw happens ✓
- Frame N+1: update() disables immediately → Draw phase sees disabled camera ✗
- Camera never actually renders!

---

## The Solution

**Key Insight**: Don't disable groups at the start of `update()`. Instead, have each RTT operation (stampFootprint, blitTexture, applyDecay) explicitly manage ALL group masks.

### New Flow:

**Frame N:**
1. `update()` START: No disabling happens
2. `update()` MIDDLE: `stampFootprint()` enables camera+footprint, disables blit+decay
3. `update()` END: Returns (camera+footprint enabled)
4. **Draw phase**: PRE_RENDER camera executes, renders footprint ✓

**Frame N+1:**
1. `update()` START: No disabling happens (camera+footprint still enabled from Frame N)
2. `update()` MIDDLE: Either:
   - `stampFootprint()` again → re-enables camera+footprint (no problem)
   - `applyDecay()` → enables camera+decay, **disables footprint+blit**
3. `update()` END: Returns
4. **Draw phase**: PRE_RENDER camera executes with correct groups ✓

---

## Code Changes

### File: `components/terrain/snowdeformation.cpp`

#### 1. Removed Group Disabling from update() Start (Lines 368-380)

**Before:**
```cpp
// Disable all RTT groups from previous frame (cleanup)
if (mBlitGroup)
    mBlitGroup->setNodeMask(0);
if (mFootprintGroup)
    mFootprintGroup->setNodeMask(0);
if (mDecayGroup)
    mDecayGroup->setNodeMask(0);
```

**After:**
```cpp
// CRITICAL TIMING FIX:
// Do NOT disable groups here at the start of update()!
//
// OSG Frame Flow:
//   Frame N: Update (enable camera) → Draw (PRE_RENDER executes)
//   Frame N+1: Update → Draw
//
// If we disable at START of Frame N+1 update, the camera is disabled
// before Frame N's draw phase completes, so it never renders!
//
// Solution: Disable/enable happens within stampFootprint/blit/decay.
// Each function explicitly disables the OTHER groups and enables its own.
// This ensures only one group is active at a time without premature disabling.
```

#### 2. blitTexture() - Added Explicit Group Disabling (Lines 940-944)

**Before:**
```cpp
// Enable blit rendering for this frame
mBlitGroup->setNodeMask(~0u);
mRTTCamera->setNodeMask(~0u);

// Other groups already disabled at start of update()
```

**After:**
```cpp
// Enable blit rendering for this frame
mBlitGroup->setNodeMask(~0u);
mRTTCamera->setNodeMask(~0u);

// Disable other groups to ensure only blit renders
if (mFootprintGroup)
    mFootprintGroup->setNodeMask(0);
if (mDecayGroup)
    mDecayGroup->setNodeMask(0);
```

#### 3. applyDecay() - Added Explicit Group Disabling (Lines 992-996)

**Before:**
```cpp
// Enable decay rendering for this frame
mDecayGroup->setNodeMask(~0u);
mRTTCamera->setNodeMask(~0u);

// Other groups already disabled at start of update()
```

**After:**
```cpp
// Enable decay rendering for this frame
mDecayGroup->setNodeMask(~0u);
mRTTCamera->setNodeMask(~0u);

// Disable other groups to ensure only decay renders
if (mFootprintGroup)
    mFootprintGroup->setNodeMask(0);
if (mBlitGroup)
    mBlitGroup->setNodeMask(0);
```

#### 4. stampFootprint() - Already Had Explicit Disabling (Lines 629-633)

This was already correct:
```cpp
// Enable RTT rendering to stamp footprint
mRTTCamera->setNodeMask(~0u);
mFootprintGroup->setNodeMask(~0u);

// Disable other groups to ensure only footprint renders
if (mBlitGroup)
    mBlitGroup->setNodeMask(0);
if (mDecayGroup)
    mDecayGroup->setNodeMask(0);
```

---

## Testing Instructions

### Build and Run:
```bash
cmake --build build --target openmw
cd build
./openmw
```

### Expected Behavior:

1. **Walk on snow terrain** (system activates everywhere for testing)
2. **Check logs** for these messages:

```
[SNOW] RTT camera created...
[SNOW] Deformation system activated
[SNOW UPDATE] distanceMoved=... willStamp=true
[SNOW TRAIL] Stamping footprint at world pos...
[SNOW DIAGNOSTIC] === Footprint Stamp #1 ===
[SNOW DIAGNOSTIC] RTT Camera node mask: 4294967295
[SNOW DIAGNOSTIC] Footprint group node mask: 4294967295
[SNOW DIAGNOSTIC] Current texture attached: 1
[SNOW] Footprint stamped, count=1 RTT camera enabled=1 Footprint group enabled=1
```

3. **Check saved textures**:
   - `snow_footprint_5.png` - Should be created after 5 footprints
   - `snow_footprint_10.png` - After 10 footprints
   - `snow_footprint_20.png` - After 20 footprints

4. **Check diagnostic output**:
```
[SNOW DIAGNOSTIC] Texture saved: snow_footprint_5.png (size=4194304 bytes)
[SNOW DIAGNOSTIC] Max depth in texture: 1.0 ← SHOULD BE 1.0, NOT 0!
```

### Success Criteria:

✅ **Max depth = 1.0** (not 0) - The quad is rendering!
✅ **No lag** - Readback callback executes only once
✅ **No crashes** - Safe readback implementation
✅ **Files created** - Not 0 bytes, contains red pixels

### If Still Failing (Max depth = 0):

This would indicate a different problem:
- Camera projection/view matrix issue
- Quad geometry issue (wrong position/plane)
- Shader not compiling
- Frame buffer attachment issue

---

## Why This Should Work Now

### The Three Critical Fixes Together:

1. **Removed premature disabling** from update() start
2. **Each function manages all masks** explicitly
3. **Only one group active per frame** (priority system in update())

### Frame-by-Frame Example:

**Frame 1:**
- `stampFootprint()` enables camera+footprint, disables blit+decay
- Draw: Camera renders red quad to texture ✓

**Frame 2:**
- No new footprint (player hasn't moved far enough)
- `applyDecay()` enables camera+decay, disables footprint+blit
- Draw: Camera renders decay shader to texture ✓

**Frame 3:**
- Player moved enough
- `stampFootprint()` enables camera+footprint, disables blit+decay
- Draw: Camera renders red quad again ✓

At no point are groups disabled before they can render!

---

## Next Steps After Confirming RTT Works

Once we see **Max depth = 1.0** in logs:

1. **Restore proper footprint shader** (replace test red shader with actual footprint calculation)
2. **Verify ping-pong buffer swapping** works correctly
3. **Test blit system** (texture following player)
4. **Test decay system** (trails fading over time)
5. **Verify in-game visuals** (terrain deformation appears)
6. **Performance optimization** if needed

---

## Summary

The RTT camera timing issue was subtle but critical. OSG's frame flow meant that disabling groups at the start of update() prevented the PREVIOUS frame's enabled camera from rendering in the CURRENT frame's draw phase.

The fix: Let each RTT operation manage all masks explicitly, ensuring the camera stays enabled long enough to render in the draw phase, while still preventing multiple operations from rendering simultaneously.

**Expected Result**: Max depth should change from 0 to 1.0, proving the quad is rendering correctly.

---

**Document Status**: Ready for Testing
**Expected Test Result**: Max depth = 1.0 (success!)
