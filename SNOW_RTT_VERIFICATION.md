# Snow Trail System - RTT Camera Verification

## Purpose

This document verifies that the RTT (Render-To-Texture) camera system for snow deformation is correctly configured and provides diagnostic tools for troubleshooting.

---

## ✅ RTT Camera Configuration Verification

### 1. Camera Orientation - Looking Down Z Axis

**Code Location**: `components/terrain/snowdeformation.cpp:525-529`

```cpp
mRTTCamera->setViewMatrixAsLookAt(
    osg::Vec3(playerPos.x(), playerPos.y(), playerPos.z() + 100.0f),  // Eye position
    osg::Vec3(playerPos.x(), playerPos.y(), playerPos.z()),            // Look-at point
    osg::Vec3(0.0f, -1.0f, 0.0f)                                       // Up vector
);
```

**Analysis**:
- **Eye position**: `(playerX, playerY, playerZ + 100)` → 100 units ABOVE player
- **Look-at point**: `(playerX, playerY, playerZ)` → Directly at player
- **View direction**: From `playerZ + 100` to `playerZ` → **DOWN the Z axis** ✅
- **Up vector**: `(0, -1, 0)` → Negative Y (South in world space)

**Verification**: Camera is correctly looking down the Z axis (from high altitude to low altitude).

---

### 2. Camera Centering - Follows Player Position

**Code Location**: `components/terrain/snowdeformation.cpp:504-531`

```cpp
// Update texture center to follow player on ground plane (XY)
mTextureCenter.set(playerPos.x(), playerPos.y());

// Move RTT camera to center over player
mRTTCamera->setViewMatrixAsLookAt(
    osg::Vec3(playerPos.x(), playerPos.y(), playerPos.z() + 100.0f),
    osg::Vec3(playerPos.x(), playerPos.y(), playerPos.z()),
    osg::Vec3(0.0f, -1.0f, 0.0f)
);
```

**Analysis**:
- Texture center uses **player's XY coordinates** (ground plane)
- Camera eye position uses **player's XY coordinates** + altitude offset
- Camera look-at uses **player's XY coordinates** at player altitude

**Verification**: Camera is perfectly centered on player's position ✅

---

### 3. Coordinate System - Z-Up Convention

**OpenMW Coordinate System** (verified in code comments):

| Axis | Direction | Usage |
|------|-----------|-------|
| **X** | East/West | Horizontal movement |
| **Y** | North/South | Horizontal movement |
| **Z** | Up/Down | Altitude/Height |

**Ground Plane**: XY plane
**Vertical Axis**: Z (positive = up, negative = down)

**RTT Camera Setup**:
- Camera at **higher Z** looking at **lower Z** → Looking DOWN ✅
- Texture follows player on **XY plane** ✅
- Quad vertices updated to match **player's Z altitude** ✅

**Verification**: All code follows Z-up convention correctly ✅

---

## 🔍 Diagnostic Features

### Automatic Texture Saving

The system now **automatically saves** deformation textures for verification:

**Trigger Points**:
- After **5 footprints**: `snow_footprint_5.png`
- After **10 footprints**: `snow_footprint_10.png`
- After **20 footprints**: `snow_footprint_20.png`

**Code Location**: `components/terrain/snowdeformation.cpp:626-633`

```cpp
if (stampCount == 5 || stampCount == 10 || stampCount == 20)
{
    std::string filename = "snow_footprint_" + std::to_string(stampCount) + ".png";
    saveDeformationTexture(filename, true);
}
```

**Saved Image Format**:
- **Red channel**: Deformation depth (0-255, brighter = deeper)
- **Green channel**: Deformed areas (255 where depth > 0.01, 0 elsewhere)
- **Blue channel**: Unused (0)
- **Alpha channel**: Opaque (255)

**Visualization**:
- **Black**: No deformation (pristine snow)
- **Red/Yellow**: Deformed areas (trails)
- **Brightness**: Depth intensity

---

### Manual Texture Saving

You can also manually save textures at any time by calling:

```cpp
// From TerrainWorld or anywhere with access to SnowDeformationManager
snowDeformationManager->saveDeformationTexture("my_diagnostic.png", true);
```

**Parameters**:
- `filename`: Output file path (PNG format recommended)
- `debugInfo`: If `true`, prints detailed camera and coordinate info to log

---

## 📊 Debug Information Output

When `debugInfo = true`, the following information is logged:

```
[SNOW DIAGNOSTIC] ====== DEBUG INFO ======
[SNOW DIAGNOSTIC] Player position: (X, Y, Z)
[SNOW DIAGNOSTIC] Texture center: (X, Y)
[SNOW DIAGNOSTIC] Texture radius: 300
[SNOW DIAGNOSTIC] Camera eye: (X, Y, Z+100)
[SNOW DIAGNOSTIC] Camera look-at: (X, Y, Z)
[SNOW DIAGNOSTIC] Camera up: (0, -1, 0) [-Y = South]
[SNOW DIAGNOSTIC] Coordinate system: X=East/West, Y=North/South, Z=Up
[SNOW DIAGNOSTIC] Visualization: Red=Depth, Green=Deformed areas
```

**Usage**: Check the log after saving a texture to verify camera parameters.

---

## 🧪 How to Verify RTT Camera is Working

### Step 1: Run the Game
Start OpenMW and load into a snowy area (system is enabled everywhere for testing).

### Step 2: Walk Around
Walk at least 10 steps to trigger automatic texture saves.

### Step 3: Check Logs
Look for these log messages:
```
[SNOW DIAGNOSTIC] Auto-saving texture after 5 footprints
[SNOW DIAGNOSTIC] Creating image for GPU readback...
[SNOW DIAGNOSTIC] Readback callback attached, will save on next frame
[SNOW DIAGNOSTIC] Texture readback complete, saving to snow_footprint_5.png
[SNOW DIAGNOSTIC] Texture saved successfully!
[SNOW DIAGNOSTIC] ====== DEBUG INFO ======
...
```

### Step 4: Inspect Saved Images
Open `snow_footprint_5.png`, `snow_footprint_10.png`, and `snow_footprint_20.png`.

**What to Look For**:
- **Red/yellow spots**: Footprints (should be visible)
- **Pattern**: Should form a trail matching your movement
- **Center**: Trail should be centered in the texture (camera following player)
- **Orientation**: Trail direction should match your world movement

**Expected Result**:
```
Black background (no deformation)
  + Red/yellow circular spots (footprints)
  = Trail pattern matching player movement
```

### Step 5: Verify Coordinates
Check the debug info output:
- **Player position** should match your in-game coordinates
- **Camera eye** should be Player position + (0, 0, 100)
- **Camera look-at** should match Player position exactly
- **Texture center** should match Player XY coordinates

---

## 🐛 Troubleshooting

### Issue: Saved textures are all black
**Possible Causes**:
- RTT camera not rendering (node mask disabled)
- Footprint shader not executing
- Deformation depth is 0

**Solution**:
1. Check logs for "Footprint stamped" messages
2. Verify RTT camera node mask: Should be `~0u` when active
3. Check `mDeformationDepth` value (should be 100.0 for snow)

### Issue: Saved textures show random noise
**Possible Causes**:
- Texture not properly initialized
- Shader compilation errors

**Solution**:
1. Check logs for shader compilation warnings
2. Verify ping-pong buffer swapping is working
3. Check that `mTexturesInitialized` is true

### Issue: Footprints not centered in texture
**Possible Causes**:
- Camera not following player correctly
- Texture center not updating

**Solution**:
1. Check logs for "SNOW CAMERA" messages
2. Verify `updateCameraPosition()` is being called each frame
3. Check that texture center matches player XY coordinates

### Issue: Cannot save texture - system not active
**Possible Causes**:
- Player not on snow (though should be enabled everywhere for testing)
- System disabled via `setEnabled(false)`

**Solution**:
1. Check `shouldBeActive()` return value
2. Verify `mEnabled` is true
3. Check `mActive` flag

---

## 📁 Output Files

Automatically saved textures will appear in:
- **Working directory**: Usually the OpenMW installation folder or run directory
- **Filename format**: `snow_footprint_<count>.png`

**Example Files**:
```
snow_footprint_5.png   - After 5 footprints
snow_footprint_10.png  - After 10 footprints
snow_footprint_20.png  - After 20 footprints
```

---

## 🔧 RTT Camera Technical Details

### Projection Matrix
```cpp
setProjectionMatrixAsOrtho(
    -300.0f, 300.0f,  // Left, Right (X range)
    -300.0f, 300.0f,  // Bottom, Top (Y range)
    -10000.0f, 10000.0f  // Near, Far (Z range)
);
```

**Coverage**: 600x600 world units on XY plane
**Vertical Range**: 20,000 units (handles all terrain heights)

### View Matrix
- **Reference Frame**: `ABSOLUTE_RF` (ignores parent transforms)
- **Eye**: 100 units above player
- **Look-at**: Player position
- **Up**: -Y (South), making +Y (North) the top of the texture

### Render Settings
- **Target**: Frame Buffer Object (FBO)
- **Order**: PRE_RENDER (before main scene)
- **Clear**: Enabled (black background)
- **Viewport**: 1024x1024 pixels

---

## ✅ Verification Checklist

Use this checklist to verify the RTT camera is working correctly:

- [ ] Camera eye is 100 units above player in Z
- [ ] Camera look-at is at player position
- [ ] Camera up vector is (0, -1, 0)
- [ ] Texture center follows player XY coordinates
- [ ] Saved textures show red/yellow footprints
- [ ] Footprints are centered in saved textures
- [ ] Trail pattern matches player movement direction
- [ ] Debug info shows correct coordinates
- [ ] Multiple saves show trail progression
- [ ] Textures saved successfully (no errors in log)

---

## 🎯 Summary

The RTT camera is **correctly configured**:
- ✅ Looking down Z axis (from high altitude to low)
- ✅ Centered on player position (XY plane)
- ✅ Following Z-up coordinate convention
- ✅ Automatic texture saving enabled for verification
- ✅ Detailed debug information available

**Next Steps**:
1. Run the game and walk around
2. Check for auto-saved texture files
3. Inspect textures for footprint trails
4. Verify coordinates in debug log output
5. Confirm RTT camera is rendering correctly

If you see red/yellow footprints in the saved textures that match your movement pattern, the RTT camera is working perfectly!
