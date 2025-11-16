# Snow Deformation System - Diagnostic Guide

## Current Issue: No Visible Deformation

### Quick Diagnostic Steps

Follow these steps **in order** to identify where the problem is:

---

## Step 1: Is the shader even running?

**Edit:** `files/shaders/compatibility/terrain.vert` line 170

**Uncomment this line:**
```glsl
if (snowDeformationEnabled) vertex.z += 500.0;
```

**Expected result:**
- ALL terrain should rise by 500 units (VERY obvious - like mountains everywhere)
- If you don't see this, the shader isn't running or `snowDeformationEnabled` is false

**If it works:** Comment it back out and go to Step 2
**If it doesn't work:** See "Shader Not Running" section below

---

## Step 2: Is snowDeformationEnabled being set to true?

**Check the logs** when you load the game. Look for:
```
[SNOW UPDATER] Binding deformation texture at (...) ENABLED=TRUE
```

**If you see ENABLED=FALSE:**
- The system is disabled or not active
- Check: Is the SnowDeformationManager created? Look for `[SNOW] SnowDeformationManager created`
- Check: Is it active? Look for `[SNOW] Deformation system activated`

**If you don't see these logs at all:**
- The updater isn't being called
- The terrain world might not have a snow deformation manager

---

## Step 3: Are footprints being stamped?

**Check the logs** when you walk around. Look for:
```
[SNOW] Stamping footprint at (X, Y) with depth=100, radius=60
[SNOW] Footprint stamped successfully (RTT enabled), count=N
```

**If you DON'T see these logs:**
- The update() function isn't being called
- The system isn't active (check shouldBeActive)
- You haven't moved far enough (needs 2+ units of movement)

**If you see these logs:**
- Footprints ARE being stamped
- The problem is in rendering (shader or uniforms)

---

## Step 4: Is snowRaiseAmount being passed correctly?

**Check the logs** for:
```
[SNOW UPDATER] Binding deformation texture at (...) raiseAmount=100
```

**If raiseAmount=0 or missing:**
- The uniform isn't being set
- The getDeformationParams function might be returning wrong values

**If raiseAmount=100:**
- The uniform is being set correctly
- Problem is likely in the shader or texture sampling

---

## Step 5: Test with simple deformation (no raising)

**Edit:** `files/shaders/compatibility/terrain.vert` line 188

**Replace:**
```glsl
vertex.z += snowRaiseAmount - deformationDepth;
```

**With:**
```glsl
vertex.z -= deformationDepth;  // Simple pits - no raising
```

**Expected result:**
- You should see HOLES/PITS where you walk
- Pits will be 100 units deep
- No raised terrain, just depressions

**If you see pits:**
- The deformation texture IS working
- The problem was the raise amount (snowRaiseAmount might be 0)

**If you don't see pits:**
- The deformation texture is not being sampled
- The texture might be empty
- UVs might be wrong

---

## Step 6: Test if texture has data

**Add this after line 184** in terrain.vert:
```glsl
float deformationDepth = texture2D(snowDeformationMap, deformUV).r;
if (deformationDepth > 0.01) vertex.z += 500.0;  // Massive spike if texture has data
```

**Expected result:**
- Massive terrain spikes where footprints exist
- If you see spikes: texture HAS data, problem is in the formula
- If you don't see spikes: texture is empty (footprints not being stamped into texture)

---

## Common Problems & Solutions

### Problem: Shader Not Running

**Symptoms:** Step 1 test doesn't raise terrain

**Possible causes:**
1. Shader not compiled or linked correctly
2. Terrain uses different shader variant
3. Multiple terrain shaders exist

**Solution:**
- Check OpenMW logs for shader compilation errors
- Look for "terrain.vert" in shader cache
- Check if @defines affect which code path is used

---

### Problem: System Not Active

**Symptoms:** No `[SNOW] Deformation system activated` log

**Possible causes:**
1. `shouldBeActive()` returns false
2. `SnowDetection::hasSnowAtPosition()` not working
3. System is disabled globally

**Solution:**
- Check line 381 in snowdeformation.cpp
- Currently hardcoded to `true` for testing
- If changed back to texture detection, it won't activate

**Quick fix:**
```cpp
// Line 381 - Force enable for testing
onSnow = true;  // TEMPORARY for testing
```

---

### Problem: Footprints Stamped But Not Visible

**Symptoms:** You see stamping logs but no visual change

**Possible causes:**
1. `snowRaiseAmount` uniform is 0
2. Deformation depth is too small (< 1 unit)
3. Texture sampler is bound to wrong texture unit
4. UVs are calculated wrong

**Debug:**
```glsl
// Add this to shader to test UV calculation
vec2 deformUV = (relativePos / snowDeformationRadius) * 0.5 + 0.5;
if (deformUV.x >= 0.4 && deformUV.x <= 0.6 && deformUV.y >= 0.4 && deformUV.y <= 0.6)
    vertex.z += 500.0;  // Spike in center of deformation area
```

If you see a spike following you, UVs are correct.

---

### Problem: Terrain Raises But No Trails

**Symptoms:** All terrain is raised uniformly, but walking doesn't create trails

**Possible causes:**
1. Footprints are being stamped but texture isn't updating
2. RTT camera not rendering
3. Ping-pong buffers not swapping

**Debug:**
- Check if stampCount increases in logs
- Add texture save code to verify texture contents
- Check if `mCurrentTextureIndex` alternates (0, 1, 0, 1...)

---

### Problem: "Pits" Work But "Raise" Doesn't

**Symptoms:** `vertex.z -= deformationDepth` creates pits, but `vertex.z += snowRaiseAmount - deformationDepth` doesn't

**Cause:** `snowRaiseAmount` is 0 or not being passed

**Solution:**
1. Check logs for `raiseAmount=100`
2. Verify `mRaiseAmountUniform` is created in updater
3. Verify uniform is added to stateset
4. Check shader has `uniform float snowRaiseAmount;` declared

**Quick test:**
```glsl
vertex.z += 100.0 - deformationDepth;  // Hardcode raise amount
```

If this works, the uniform isn't being passed.

---

## What SHOULD Happen (Correct Behavior)

### When System is Working:

1. **Terrain raises uniformly by 100 units** where snow deformation is enabled
2. **Character walks on TOP of raised terrain** (waist-deep in fluffy snow)
3. **Footprints create 100-unit depressions** down to original terrain height
4. **Trails persist** as you walk away (visible behind you)
5. **Trails gradually fade** over 120 seconds (decay system)
6. **Trails move with you** when you move far (blit system)

### Visual Appearance:

- **Untouched snow:** Raised platform ~waist height
- **Footprint trail:** Depression showing original terrain
- **Looks like:** Plowing through deep snow, leaving a trench
- **Character appears:** Walking ON the snow, not sinking into terrain

---

## Quick Reference: Key Values

```cpp
// C++ (snowdeformation.cpp)
mDeformationDepth = 100.0f;        // How deep footprints go
mFootprintRadius = 60.0f;          // How wide footprints are
mFootprintInterval = 2.0f;         // Distance between stamps
mWorldTextureRadius = 300.0f;      // Texture coverage area
mTextureResolution = 1024;         // Deformation texture size

// Shader (terrain.vert)
uniform float snowRaiseAmount;     // Should be 100.0 (from C++)
uniform float snowDeformationRadius; // Should be 300.0
```

---

## Next Steps

1. **Run Step 1** - Verify shader runs
2. **Run Step 2** - Verify system is enabled
3. **Run Step 3** - Verify footprints are stamped
4. **Run Step 4** - Verify raise amount is passed
5. **Run Step 5** - Test simple pits (no raise)
6. **Report results** - Tell me which step fails!

---

## Most Likely Causes

Based on your description ("it worked in the past but had no trails"):

**Most likely:** The old code created simple pits (`vertex.z -= depth`) which worked, but:
- ❌ Pits disappeared when you moved (no blit system)
- ❌ No raised terrain (just holes)
- ❌ No persistent trails

**Current issue:** The raise system should work but might be:
- `snowRaiseAmount` uniform not being set (value = 0)
- System not active (`snowDeformationEnabled` = false)
- Shader not being used (different shader variant)

**Quick fix to test:** Uncomment line 170 diagnostic test and run the game!
