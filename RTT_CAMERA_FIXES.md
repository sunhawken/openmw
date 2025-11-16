# RTT Camera Critical Fixes - Snow Deformation System

## Date: 2025-11-16

## Problem Summary
The RTT (Render-To-Texture) camera was not rendering footprints to the deformation texture. Symptoms:
- Terrain raised by 100 units (shader working)
- No trails/footprints visible
- Saved texture files were 0 bytes
- `success=0` when saving images

## Root Causes Identified

### 1. **Missing ABSOLUTE_RF Reference Frame** (CRITICAL!)
**File:** `components/terrain/snowdeformation.cpp:91`

**Problem:** RTT camera was inheriting view/projection matrices from parent scene node, completely breaking the orthographic top-down view.

**Fix:**
```cpp
mRTTCamera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
```

**Impact:** Without this, the camera's carefully crafted orthographic projection was being overridden by the main camera's perspective projection and view transforms.

---

### 2. **Image Attached to RTT Texture** (CRITICAL!)
**File:** `components/terrain/snowdeformation.cpp:146-155`

**Problem:** We were calling `setImage()` on the texture to initialize it to zeros. OSG doesn't update CPU-side images from RTT renders, so saves always showed the original zeros.

**Old code:**
```cpp
osg::ref_ptr<osg::Image> image = new osg::Image;
image->allocateImage(...);
// ... initialize to zeros ...
mDeformationTexture[i]->setImage(image);  // BAD for RTT!
```

**Fix:** Removed setImage() entirely. RTT textures are GPU-only.

**New approach:** Initialize via clear on first activation (lines 328-348)

---

### 3. **Camera Orientation Wrong for Z-Up**
**File:** `components/terrain/snowdeformation.cpp:100-104`

**Problem:** "Up" vector was `(0, 1, 0)` which is ambiguous when looking straight down the Z-axis.

**Fix:**
```cpp
mRTTCamera->setViewMatrixAsLookAt(
    osg::Vec3(0.0f, 0.0f, 100.0f),  // Eye (Z+ is up)
    osg::Vec3(0.0f, 0.0f, 0.0f),    // Look at origin
    osg::Vec3(0.0f, -1.0f, 0.0f)    // Up = -Y (South)
);
```

---

### 4. **Quad Geometry in Wrong Plane**
**Files:**
- `components/terrain/snowdeformation.cpp:183-186` (footprint quad)
- `components/terrain/snowdeformation.cpp:543-546` (blit quad)
- `components/terrain/snowdeformation.cpp:635-638` (decay quad)

**Problem:** Quads were in XZ plane `(X, 0, Z)` but OpenMW uses XY as ground plane (Z is up).

**Old:**
```cpp
vertices->push_back(osg::Vec3(-radius, 0.0f, -radius));  // XZ plane, Y=0
```

**Fix:**
```cpp
vertices->push_back(osg::Vec3(-radius, -radius, 0.0f));  // XY plane, Z=0
```

---

### 5. **Texture Initialization**
**File:** `components/terrain/snowdeformation.cpp:326-348`

**Problem:** With no setImage(), textures had undefined contents on first use.

**Fix:** Added initialization on first activation:
```cpp
if (!mTexturesInitialized)
{
    // Clear texture 0
    mRTTCamera->attach(osg::Camera::COLOR_BUFFER, mDeformationTexture[0].get());
    mRTTCamera->setNodeMask(~0u);
    mRTTCamera->frame();  // Force render with clear

    // Clear texture 1
    mRTTCamera->attach(osg::Camera::COLOR_BUFFER, mDeformationTexture[1].get());
    mRTTCamera->frame();  // Force render with clear

    mTexturesInitialized = true;
}
```

---

## Diagnostic Improvements

### Comprehensive Logging
**File:** `components/terrain/snowdeformation.cpp:527-613`

Added:
- RTT camera setup parameters
- Per-footprint logging with counts
- Texture attachment verification
- GPU readback via DrawCallback for texture saves
- Max depth analysis in saved textures

### Texture Saving
**File:** `components/terrain/snowdeformation.cpp:565-598`

Saves deformation texture at stamps 3, 10, and 50 using:
```cpp
struct SaveCallback : public osg::Camera::DrawCallback
{
    void operator()(osg::RenderInfo& renderInfo) const override
    {
        image->readPixels(0, 0, resolution, resolution, GL_RGBA, GL_FLOAT);
        osgDB::writeImageFile(*image, filename);

        // Analyze max depth
        float maxDepth = 0.0f;
        for (int i = 0; i < resolution * resolution; ++i)
            maxDepth = std::max(maxDepth, data[i * 4]);

        Log(Debug::Info) << "Max depth in texture: " << maxDepth;
    }
};
```

---

## Testing Instructions

1. Build OpenMW with these changes
2. Run and walk on snow terrain
3. Check log output:
   - `[SNOW] RTT camera created` - verify FBO=1, render order, clear mask
   - `[SNOW] Footprint stamping setup` - verify geometry counts
   - `[SNOW] Footprint stamped, count=X` - verify stamps happening
   - `[SNOW DIAGNOSTIC CALLBACK]` - verify texture saves and max depth
4. Check for files:
   - `snow_deform_readback_3.png`
   - `snow_deform_readback_10.png`
   - `snow_deform_readback_50.png`
5. Verify files are NOT 0 bytes
6. Log should show `Max depth in texture: <positive number>`

---

## Expected Behavior After Fixes

### What Should Happen:
1. RTT camera renders footprint quads to deformation texture
2. Texture accumulates depth values via ping-pong shader
3. Terrain shader samples texture and applies deformation
4. Visible trails appear where player walks
5. Saved textures show white circles (footprints) on black background

### What Should NOT Happen:
- ❌ 0-byte texture files
- ❌ All-raised terrain with no trails
- ❌ Max depth = 0.0 in diagnostics
- ❌ RTT camera inheriting wrong transforms

---

## Summary of Changed Files

### Core Fixes:
- `components/terrain/snowdeformation.cpp` - All RTT setup and rendering
- `components/terrain/snowdeformation.hpp` - Added mTexturesInitialized flag

### Lines Changed:
- Line 91: Added ABSOLUTE_RF
- Lines 100-104: Fixed camera up vector
- Lines 109-110: Enabled clearing
- Lines 146-155: Removed setImage()
- Lines 183-186, 543-546, 635-638: Fixed quad planes
- Lines 326-348: Added texture initialization
- Lines 527-613: Comprehensive diagnostics

---

## Why RTT Was Failing

The combination of issues created a perfect storm:

1. **No ABSOLUTE_RF** → Camera using wrong view/projection → Nothing rendered correctly
2. **setImage() on RTT texture** → CPU image never updated from GPU → 0-byte saves
3. **Wrong quad plane** → Geometry not visible to top-down camera
4. **Wrong up vector** → Texture orientation potentially flipped

Any ONE of these could prevent RTT from working. All together made it completely broken.

---

## Next Steps

After confirming RTT works:
1. Verify footprint shader math (radius, falloff, depth)
2. Check ping-pong buffer swapping
3. Test blit system for texture scrolling
4. Test decay system
5. Optimize performance
6. Fine-tune parameters (depth, radius, etc.)
