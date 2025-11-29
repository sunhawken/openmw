# Snow Deformation System - Methodical Debug Plan

**Problem:** Still seeing full square area being depressed instead of individual footprints
**Status:** CullCallback attached successfully (log confirms), but bug persists
**Date:** 2025-11-29

---

## Debug Strategy Overview

We need to trace the data flow through the entire pipeline to find where it breaks:

```
Player Position → Depth Camera → Object Mask Texture → Update Shader →
Accumulation Buffer → Blur H → Blur V → Blurred Map → Terrain Shaders → Visual Result
```

We'll add debugging at each stage to see exactly what data exists where.

---

## Phase 1: Verify Object Mask Texture Content

**Goal:** Confirm what the depth camera is actually rendering

### Step 1.1: Add Texture Dump Code

Add this method to `SnowDeformationManager` class:

**File:** `components/terrain/snowdeformation.hpp`

```cpp
// In public methods section (around line 88):
void debugDumpTexture(const std::string& filename, osg::Texture2D* texture) const;
```

**File:** `components/terrain/snowdeformation.cpp`

Add after `addFootprintToRTT()` method (around line 854):

```cpp
void SnowDeformationManager::debugDumpTexture(const std::string& filename, osg::Texture2D* texture) const
{
    if (!texture) return;

    osg::Image* image = texture->getImage();
    if (!image)
    {
        // Texture might not have an image attached (FBO target)
        // We need to read it from GPU
        image = new osg::Image;
        image->allocateImage(
            texture->getTextureWidth(),
            texture->getTextureHeight(),
            1,
            GL_RGBA,
            GL_UNSIGNED_BYTE);

        // This requires a valid OpenGL context, call during rendering
        Log(Debug::Warning) << "Cannot dump texture without GPU readback - needs implementation";
        return;
    }

    std::string fullPath = "d:\\Gamedev\\OpenMW\\openmw-dev-master\\" + filename;
    if (osgDB::writeImageFile(*image, fullPath))
    {
        Log(Debug::Info) << "DEBUG: Dumped texture to " << fullPath;
    }
    else
    {
        Log(Debug::Error) << "DEBUG: Failed to dump texture to " << fullPath;
    }
}
```

### Step 1.2: Add Logging to CullCallback

**File:** `components/terrain/snowdeformation.cpp` (lines 45-64)

```cpp
// Replace the callback's operator() with this version:
virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
{
    traverse(node, nv);

    if (mRoot && nv)
    {
        int childrenTraversed = 0;
        int childrenSkipped = 0;

        for (unsigned int i = 0; i < mRoot->getNumChildren(); ++i)
        {
            osg::Node* child = mRoot->getChild(i);
            std::string childName = child->getName().empty() ? "<unnamed>" : child->getName();

            if (child == mCam)
            {
                Log(Debug::Verbose) << "  [SKIP] Camera itself";
                childrenSkipped++;
                continue;
            }

            if (childName == "Terrain Root")
            {
                Log(Debug::Verbose) << "  [SKIP] Terrain Root";
                childrenSkipped++;
                continue;
            }

            // Log what we're about to traverse
            unsigned int nodeMask = child->getNodeMask();
            Log(Debug::Info) << "  [TRAVERSE] " << childName
                            << " (mask: 0x" << std::hex << nodeMask << std::dec << ")";
            childrenTraversed++;
            child->accept(*nv);
        }

        Log(Debug::Info) << "DepthCameraCullCallback: Traversed " << childrenTraversed
                        << " nodes, skipped " << childrenSkipped;
    }
}
```

### Step 1.3: Test and Check Logs

**Expected Logs:**
```
[Info] SnowDeformationManager: Attached DepthCameraCullCallback to depth camera
[Info]   [TRAVERSE] Player Root (mask: 0xffffffff)
[Info]   [TRAVERSE] Cell Root (mask: 0xffffffff)
[Info]   [SKIP] Terrain Root
[Info] DepthCameraCullCallback: Traversed 2 nodes, skipped 2
```

**Analysis:**
- If you see 0 nodes traversed → Scene graph structure is wrong
- If you see traversed nodes but still no footprints → Node mask filtering is failing deeper in hierarchy
- If you see "Sky Root", "Water Root" being traversed → That's the problem!

---

## Phase 2: Visual Debug in Fragment Shader

**Goal:** See what values are in the deformation textures

### Step 2.1: Enable Direct Texture Visualization

**File:** `files/shaders/compatibility/terrain.frag`

Replace the commented-out debug code (lines 65-71) with this enhanced version:

```glsl
// DEBUG: Visualize Snow RTT - ENHANCED
vec2 debugUV = (passWorldPos.xy - snowRTTWorldOrigin.xy) / snowRTTScale + 0.5;
if (debugUV.x >= 0.0 && debugUV.x <= 1.0 && debugUV.y >= 0.0 && debugUV.y <= 1.0)
{
    float deformValue = texture2D(snowDeformationMap, debugUV).r;

    // Visualize the deformation map directly
    // Green tint = inside RTT area
    // Red = deformation present
    // Blue = object mask (if we add it as a uniform)
    gl_FragData[0].rgb = mix(gl_FragData[0].rgb, vec3(0, 1, 0), 0.3); // Green tint

    if (deformValue > 0.01)
    {
        // Show deformation as red, intensity = amount
        gl_FragData[0].rgb = mix(gl_FragData[0].rgb, vec3(1, 0, 0), deformValue);
    }
    else if (deformValue < -0.01)
    {
        // Show negative values (rim) as blue
        gl_FragData[0].rgb = mix(gl_FragData[0].rgb, vec3(0, 0, 1), -deformValue);
    }

    // DEBUG: Show exact value as grayscale in corner of screen
    if (gl_FragCoord.x < 200.0 && gl_FragCoord.y < 200.0)
    {
        // Map deformation value to grayscale
        float gray = deformValue * 0.5 + 0.5; // Remap [-1,1] to [0,1]
        gl_FragData[0].rgb = vec3(gray);
    }
}
```

**What to look for:**
- **Green square following you:** RTT area is correct ✅
- **Entire green square is red:** Deformation map is full white (the bug!)
- **Red trails behind you:** Working correctly! ✅
- **No red at all:** Deformation map is empty (depth camera not working)

### Step 2.2: Add Object Mask Visualization

To see what the depth camera is actually outputting, we need to expose the object mask to the terrain shader.

**File:** `components/terrain/snowdeformation.hpp` (around line 84)

Add:
```cpp
osg::Uniform* getObjectMaskUniform() const { return mObjectMaskUniform.get(); }
osg::Texture2D* getObjectMaskMap() const { return mObjectMaskMap.get(); }
```

**File:** `components/terrain/snowdeformationupdater.cpp`

Find where uniforms are bound (likely in `apply()` method) and add:

```cpp
// Bind object mask for debugging
stateset->setTextureAttributeAndModes(8,
    mManager->getObjectMaskMap(),
    osg::StateAttribute::ON);
stateset->addUniform(new osg::Uniform("debugObjectMask", 8));
```

**File:** `files/shaders/compatibility/terrain.frag`

Add uniform and visualization:

```glsl
// After line 18:
uniform sampler2D debugObjectMask; // For debugging

// In debug section:
float maskValue = texture2D(debugObjectMask, debugUV).r;
if (maskValue > 0.5)
{
    // Show object mask as bright yellow
    gl_FragData[0].rgb = vec3(1, 1, 0);
}
```

**What to look for:**
- **Yellow blob where you're standing:** Depth camera working! ✅
- **Entire square is yellow:** Depth camera rendering everything (Sky, Water, etc.)
- **No yellow anywhere:** Depth camera rendering nothing

---

## Phase 3: Check Accumulation Buffer Data Flow

**Goal:** Verify the update shader is working correctly

### Step 3.1: Add Logging to Update Pass

**File:** `components/terrain/snowdeformation.cpp` (in `updateRTT()` method, around line 664)

Add detailed logging:

```cpp
void SnowDeformationManager::updateRTT(float dt, const osg::Vec3f& playerPos)
{
    if (!mRTTCamera || !mUpdateCamera) return;

    // Log player position and RTT state
    Log(Debug::Info) << "=== RTT Update Frame ===";
    Log(Debug::Info) << "Player pos: " << playerPos;
    Log(Debug::Info) << "RTT Center: " << mRTTCenter;
    Log(Debug::Info) << "RTT Size: " << mRTTSize;

    // ... existing offset calculation ...

    osg::Vec2 offset(delta.x() / mRTTSize, delta.y() / mRTTSize);
    mRTTOffsetUniform->set(offset);

    Log(Debug::Info) << "Offset UV: " << offset;
    Log(Debug::Info) << "Decay amount: " << decayAmount;

    // ... rest of method ...

    Log(Debug::Info) << "Buffer swap: Read=" << readIndex << " Write=" << writeIndex;
}
```

**Check logs for:**
- RTT center following player ✅
- Offset values changing as you move ✅
- Offset values are HUGE (>1.0) → You moved too far, window reset
- Decay amount reasonable (small positive number) ✅

### Step 3.2: Add Debug Output to Update Shader

**File:** `files/shaders/compatibility/snow_update.frag`

Add debug output after line 50:

```glsl
// Debug: Output diagnostic info
// Uncomment ONE of these at a time to test different stages:

// Test 1: Just pass through previous frame (no decay, no new data)
// gl_FragColor = vec4(previousValue, 0, 0, 1);

// Test 2: Just pass through new object mask
// gl_FragColor = vec4(newValue, 0, 0, 1);

// Test 3: Show max without rim (to test if rim is the issue)
// float testValue = max(previousValue, newValue);
// gl_FragColor = vec4(testValue, 0, 0, 1);

// Test 4: Show rim effect only
// float testValue = finalValue - max(previousValue, newValue);
// gl_FragColor = vec4(testValue * 10.0, 0, 0, 1); // Amplify to see

gl_FragColor = vec4(finalValue, 0.0, 0.0, 1.0); // Normal output
```

**Testing procedure:**
1. Build with Test 2 uncommented → See raw object mask
2. Walk around → Should see white blob ONLY where you stand RIGHT NOW
3. If entire square is white → Object mask is the problem!
4. Build with Test 3 → See accumulation without rim
5. If trails work without rim → Rim function is the problem!

---

## Phase 4: Verify Blur Pipeline

**Goal:** Check if blur shaders are executing correctly

### Step 4.1: Add Logging to Blur Setup

**File:** `components/terrain/snowdeformation.cpp` (in `initRTT()`, around lines 485-530)

Add after blur shader loading:

```cpp
// After line 486:
if (hFrag)
{
    hProg->addShader(hFrag);
    Log(Debug::Info) << "SnowDeformationManager: Loaded blur_horizontal.frag successfully";
}
else
{
    Log(Debug::Error) << "Failed to load blur_horizontal.frag";
}

// After line 527:
if (vFrag)
{
    vProg->addShader(vFrag);
    Log(Debug::Info) << "SnowDeformationManager: Loaded blur_vertical.frag successfully";
}
else
{
    Log(Debug::Error) << "Failed to load blur_vertical.frag";
}
```

### Step 4.2: Test Blur Bypass

To check if blur is causing issues, temporarily bypass it:

**File:** `components/terrain/snowdeformation.cpp`

Change which texture the terrain reads from (around line 85 in header):

```cpp
// Test: Read directly from accumulation buffer, skip blur
osg::Texture2D* getDeformationMap() const { return mAccumulationMap[mWriteBufferIndex].get(); }

// Normal: Read from blurred buffer
// osg::Texture2D* getDeformationMap() const { return mBlurredDeformationMap.get(); }
```

**If bypassing blur fixes it:**
- Problem is in blur shaders or blur camera setup
- Check blur shader compilation logs
- Check input texture binding

**If bypassing blur doesn't fix it:**
- Problem is earlier in pipeline (object mask or update shader)

---

## Phase 5: Check Vertex Shader Logic

**Goal:** Verify the "raise then lower" approach isn't causing issues

### Step 5.1: Add Debug Visualization for Vertex Displacement

**File:** `files/shaders/compatibility/terrain.vert`

Add after line 107 (after baseLift calculation):

```glsl
// DEBUG: Log values (appears as varying, check in fragment shader)
// vDeformationFactor already passed to fragment

// DEBUG: Force specific behavior to test
// Uncomment ONE at a time:

// Test 1: Disable all deformation
// vertex.z += 0.0;
// vMaxDepth = 0.0;

// Test 2: Simple depression (no raise/lower)
// vertex.z -= vDeformationFactor * baseLift;
// vMaxDepth = baseLift;

// Test 3: Current approach (raise then lower)
vertex.z += baseLift * (1.0 - vDeformationFactor);
vMaxDepth = baseLift;
```

### Step 5.2: Check baseLift Value

**File:** `files/shaders/compatibility/terrain.vert`

Add logging of vertex attributes (this requires debug output, or check in fragment shader):

```glsl
// Before vertex displacement (around line 85):
if (baseLift > 0.01)
{
    // This vertex is on deformable terrain
    // baseLift should be something like 10-50 units (check settings)

    // You can pass baseLift to fragment shader for visualization
    // Add: varying float vBaseLift;
    // vBaseLift = baseLift;
}
```

**File:** `files/shaders/compatibility/terrain.frag`

Add after debug section:

```glsl
// Add: varying float vBaseLift;

// Visualize base lift value
if (vBaseLift > 0.01)
{
    // Show areas where terrain is deformable as purple
    gl_FragData[0].rgb = mix(gl_FragData[0].rgb, vec3(1, 0, 1), 0.2);
}
```

**What to look for:**
- **Purple everywhere in RTT area:** baseLift is being applied everywhere (expected for your approach)
- **Purple only in specific areas:** Terrain weight system is filtering correctly
- **No purple:** baseLift is zero (settings issue or terrain weights wrong)

---

## Phase 6: Nuclear Option - Render Order Debug

**Goal:** Verify cameras are rendering in correct order

### Step 6.1: Add Frame Markers

**File:** `components/terrain/snowdeformation.cpp`

Add to each camera's clear color a unique identifier:

```cpp
// Depth Camera (Pass 0)
mDepthCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f)); // Black

// Update Camera (Pass 1) - Temporarily change for debugging
mUpdateCamera->setClearColor(osg::Vec4(0.1f, 0.0f, 0.0f, 1.0f)); // Very dark red (for 1 frame)

// Footprint Camera (Pass 2) - Don't clear!
mRTTCamera->setClearMask(0); // Correct

// Blur H (Pass 3)
mBlurHCamera->setClearColor(osg::Vec4(0.0f, 0.1f, 0.0f, 1.0f)); // Very dark green

// Blur V (Pass 4)
mBlurVCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.1f, 1.0f)); // Very dark blue
```

Then in terrain fragment shader, check for these colors:

```glsl
float r = texture2D(snowDeformationMap, debugUV).r;
if (r > 0.09 && r < 0.11)
{
    // Seeing the clear color → camera not rendering geometry!
    gl_FragData[0].rgb = vec3(1, 0, 1); // MAGENTA = ERROR
}
```

---

## Quick Diagnostic Checklist

Run through these quick checks first:

### Check 1: Settings Values
```cpp
// In SnowDeformationManager constructor, add:
Log(Debug::Info) << "Snow settings:";
Log(Debug::Info) << "  Enabled: " << mEnabled;
Log(Debug::Info) << "  Radius: " << mFootprintRadius;
Log(Debug::Info) << "  Depth: " << mDeformationDepth;
Log(Debug::Info) << "  RTT Size: " << mRTTSize;
Log(Debug::Info) << "  Decay Time: " << mDecayTime;
```

**Expected:**
- Enabled: true
- Radius: ~30-50 units
- Depth: ~10-20 units
- RTT Size: 3625.0
- Decay Time: ~180 seconds

### Check 2: Texture Formats
```cpp
// In initRTT(), add after texture creation:
Log(Debug::Info) << "Texture formats:";
Log(Debug::Info) << "  Object Mask: " << mObjectMaskMap->getInternalFormat()
                << " (" << mObjectMaskMap->getTextureWidth() << "x"
                << mObjectMaskMap->getTextureHeight() << ")";
Log(Debug::Info) << "  Accumulation: " << mAccumulationMap[0]->getInternalFormat()
                << " (" << mAccumulationMap[0]->getTextureWidth() << "x"
                << mAccumulationMap[0]->getTextureHeight() << ")";
```

**Expected:**
- Object Mask: GL_R8 (6403), 2048×2048
- Accumulation: GL_RGBA16F_ARB (34842), 2048×2048

### Check 3: Camera Masks
```cpp
// After camera creation:
Log(Debug::Info) << "Camera cull masks:";
Log(Debug::Info) << "  Depth Camera: 0x" << std::hex << mDepthCamera->getCullMask() << std::dec;
Log(Debug::Info) << "  Expected: 0x" << std::hex << ((1<<3)|(1<<4)|(1<<10)) << std::dec;
```

**Expected:**
- Depth Camera: 0x418 (1048 decimal) = bits 3, 4, 10

---

## Most Likely Culprits (Ordered by Probability)

Based on "full square depression" symptom:

### Hypothesis 1: Object Mask is Full White (MOST LIKELY)
**Symptom:** Entire RTT area shows deformation
**Cause:** Depth camera rendering Sky/Water/Everything instead of just actors
**Test:** Phase 2, Step 2.2 (visualize object mask)
**Fix:** Adjust what gets traversed in CullCallback

### Hypothesis 2: Cubic Rim Function Going Wrong
**Symptom:** Rim is spreading to entire area
**Cause:** Rim calculation creating negative→positive→negative cycle
**Test:** Phase 3, Step 3.2, Test 3 (bypass rim)
**Fix:** Adjust rim intensity or disable it

### Hypothesis 3: Blur Bleeding
**Symptom:** Small footprints blurred into large square
**Cause:** Blur radius too large or wrapping at edges
**Test:** Phase 4, Step 4.2 (bypass blur)
**Fix:** Reduce blur kernel size or fix edge clamping

### Hypothesis 4: baseLift Applied Globally
**Symptom:** Entire terrain raised, RTT area "normal" looks like depression
**Cause:** Terrain outside RTT also getting baseLift somehow
**Test:** Phase 5, Step 5.2 (visualize baseLift areas)
**Fix:** Ensure baseLift only applied inside RTT bounds

---

## Recommended Debug Sequence

**Start here (fastest to check):**

1. **Add Phase 2, Step 2.1 visual debug** (5 minutes)
   - Build, run, look at screen
   - Is green square red? → Object mask problem
   - Is green square black? → Depth camera not rendering
   - Red trails only? → Working! (shouldn't be seeing this though)

2. **Add Phase 1, Step 1.2 logging** (5 minutes)
   - Check what nodes are being traversed
   - If "Sky Root" or "Water Root" appear → That's the problem!

3. **Add Phase 2, Step 2.2 object mask viz** (10 minutes)
   - See exactly what depth camera outputs
   - Yellow blob = good, yellow square = bad, no yellow = broken

4. **If object mask is the issue, add more filtering** (15 minutes)
   - In CullCallback, skip more nodes by name:
   ```cpp
   if (childName == "Terrain Root" ||
       childName == "Sky Root" ||
       childName == "Water Root" ||
       childName.find("Camera") != std::string::npos)
   {
       continue;
   }
   ```

5. **If still broken, try Phase 3, Step 3.2 tests** (20 minutes)
   - Isolate which stage is corrupting data
   - Test bypass scenarios

---

## Success Criteria

You'll know it's fixed when:

1. **Logs show:**
   ```
   DepthCameraCullCallback: Traversed 2 nodes, skipped 2
   ```
   (Only Player Root and Cell Root, not Sky/Water)

2. **Visual debug shows:**
   - Green square following you ✅
   - Small yellow blob where you stand ✅
   - Red trail behind you as you walk ✅
   - Rest of green square is black (no deformation) ✅

3. **In-game shows:**
   - Individual footprints ✅
   - Smooth edges (blur working) ✅
   - No full-area depression ✅

---

## Emergency Fallback: Simplify Everything

If all debugging fails, try this simplified approach:

**File:** `files/shaders/compatibility/snow_update.frag`

Replace entire shader with:

```glsl
#version 120
uniform sampler2D objectMask;

void main()
{
    // Super simple: just output object mask directly, no accumulation
    vec2 uv = gl_TexCoord[0].xy;
    float mask = texture2D(objectMask, uv).r;
    gl_FragColor = vec4(mask, 0, 0, 1);
}
```

If this STILL shows full square white:
- **Object mask is definitely the problem**
- Depth camera is rendering too much geometry
- Need to fix scene traversal or add more filtering

If this shows correct small blobs:
- **Object mask is fine, problem is in accumulation/blur**
- Re-enable features one by one to find which breaks it

---

**Debug Plan Created:** 2025-11-29
**Estimated Time:** 1-3 hours depending on findings
**Next Action:** Implement Phase 1 & 2 visual debugging first
