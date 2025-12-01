# Snow Deformation Debug Plan

## Problem Statement
The RTT area shows as fully white/deformed instead of only where actors walk.
Accumulation doesn't work - trails don't persist.

---

## COMPLETED TESTS & FINDINGS

### Test 1A: Object Mask via Update Shader (debugMode=2)
**Setup:** Update shader outputs `texture2D(objectMask, uv).r` directly
**Result:** WHITE
**Conclusion:** Update shader cannot read the object mask texture on unit 1

### Test 1B: Object Mask Direct to Terrain
**Setup:** Terrain reads object mask directly (bypass entire RTT pipeline)
- Depth camera clears to MAGENTA, cull callback disabled
- `snowdeformationupdater.cpp` binds `getObjectMaskMap()` to unit 7
**Result:** MAGENTA
**Conclusion:**
- Depth camera FBO works correctly
- Terrain CAN read the object mask texture
- **Problem is in the RTT simulation pipeline** (update shader texture binding)

### Test 2: Verify Update Shader Executes (debugMode=1 for CYAN)
**Setup:**
- Changed `debugMode` to 1 in `snowsimulation.cpp`
- Restored terrain to read from RTT pipeline (`getDeformationMap()`)
**Result:** MAGENTA (not CYAN)
**Conclusion:** Terrain was still bound to object mask from Test 1B bypass

### Test 3: Fix Terrain Binding + debugMode=1
**Setup:**
- Fixed `snowdeformationupdater.cpp` to bind `manager->getDeformationMap()` instead of `getObjectMaskMap()`
- `getOutputTexture()` returns `mBlurredDeformationMap`
**Result:** WHITE
**Conclusion:** Blur passes are interfering - CYAN (R=0) gets filtered to BLACK by blur shaders that only read `.r` channel

### Test 4: Bypass Blur Passes
**Setup:**
- Changed `getOutputTexture()` to return `mAccumulationMap[mWriteBufferIndex]` directly
**Result:** CYAN for 1 frame, then WHITE
**Conclusion:**
- **Update shader IS executing correctly on frame 1!**
- Problem is with ping-pong buffer timing - terrain reads wrong buffer on subsequent frames

### Test 5: Fix Ping-Pong Read Index
**Setup:**
- Changed `getOutputTexture()` to return `mAccumulationMap[(mWriteBufferIndex + 1) % 2]` (read buffer instead of write buffer)
**Result:** WHITE
**Conclusion:** Still a timing issue between when buffers swap and when terrain reads

### Test 6: Disable Ping-Pong Entirely
**Setup:**
- Disabled buffer swapping in `update()` - always use buffer 0
- Commented out `detach()/attach()` calls
- `getOutputTexture()` returns `mAccumulationMap[0]`
**Result:** CYAN for 1 frame, then WHITE
**Conclusion:**
- **The update camera renders correctly on frame 1**
- **Something clears/overwrites the accumulation buffer after frame 1**
- This is NOT a ping-pong issue - buffer 0 is written once and then cleared

### Test 7: Add Extensive Logging
**Setup:**
- Added `[SnowSim]` logging to `update()` with frame counter, buffer indices, texture pointers
- Added `[SnowUpdater]` logging to `apply()` every 60 frames
- Added initialization logging to `createUpdatePass()`
**Result:** Logs show:
- `update()` runs BEFORE `apply()` (Frame 1 update at 10:53:41.654, apply at 10:53:41.813)
- Texture pointers are consistent across all frames
- Found warning: `Uniform numElements < 1 is invalid` during initialization
**Conclusion:** Identified bad uniform constructor usage

### Test 8: Fix Sampler Uniform Constructor
**Setup:**
- Changed from `new osg::Uniform(osg::Uniform::SAMPLER_2D, "name", value)` (array constructor)
- To `new osg::Uniform("name", value)` (simple int constructor for samplers)
**Result:** Warning gone, but still CYAN for 1 frame then WHITE
**Conclusion:** Uniform was malformed but fixing it didn't solve the core issue

### Test 9: Disable Update Camera Clear Mask
**Setup:**
- Changed `mUpdateCamera->setClearMask(GL_COLOR_BUFFER_BIT)` to `setClearMask(0)`
**Result:** Still WHITE after frame 1
**Conclusion:** The clear mask is NOT the cause of the buffer being wiped

### Test 10: Add DrawCallback to Verify Camera Renders Each Frame
**Setup:**
- Added `SnowCameraDrawCallback` to all 4 cameras (DepthCamera, UpdateCamera, BlurHCamera, BlurVCamera)
- Callback logs when each camera's draw phase completes
**Result:** ALL cameras fire their DrawCallback EVERY frame:
```
[SnowDeform] DepthCamera DrawCallback fired! Draw count: 19
[SnowSim] UpdateCamera DrawCallback fired! Draw count: 19
[SnowSim] BlurHCamera DrawCallback fired! Draw count: 19
[SnowSim] BlurVCamera DrawCallback fired! Draw count: 19
```
**Conclusion:**
- **All cameras ARE rendering every frame** - hypothesis about camera not rendering is WRONG
- The UpdateCamera definitely executes its draw pass
- Order is correct: DepthCamera → UpdateCamera → BlurH → BlurV

### Test 11: Hard-Code Shader Output to CYAN
**Setup:**
- Modified `snow_update.frag` to unconditionally output CYAN at the very start:
```glsl
void main()
{
    gl_FragColor = vec4(0.0, 1.0, 1.0, 1.0);
    return;
    // ... rest of shader never executes
}
```
**Result:** CYAN for 1 frame, then WHITE
**Conclusion:**
- **CRITICAL FINDING: The shader unconditionally outputs CYAN, yet terrain sees WHITE after frame 1**
- This means **the snow_update.frag shader is NOT what's rendering to the texture on frame 2+**
- Something else must be overwriting `mAccumulationMap[0]` between when UpdateCamera renders and when terrain samples

### Test 12: Track OpenGL Texture ID
**Setup:**
- Added GL texture ID logging to DrawCallback and SnowUpdater
- Logs the actual OpenGL texture ID (not just OSG pointer)
**Result:**
```
[SnowSim] UpdateCamera DrawCallback fired! Draw count: 19, GL TexID: 25
[SnowUpdater] apply() frame 121: binding deformationMap=00000238DF3FF280 GL TexID=25 to unit 7
```
**Conclusion:**
- OSG texture pointer AND OpenGL texture ID are consistent
- Both UpdateCamera and terrain are referencing the same GL texture (ID 25)
- The texture identity is NOT the problem

### Test 13: Remove Initial Image from Texture
**Setup:**
- Removed `setImage()` calls from accumulation textures
- Hypothesis: Initial image might conflict with FBO rendering
**Result:** Still WHITE after frame 1
**Conclusion:** Initial image is not the cause

### Test 14: Simpler Texture Format
**Setup:**
- Changed accumulation texture from `GL_RGBA16F_ARB` to `GL_RGBA`
- Changed from `GL_FLOAT` to `GL_UNSIGNED_BYTE`
**Result:** Still WHITE after frame 1
**Conclusion:** Texture format is not the cause

---

## CURRENT ROOT CAUSE HYPOTHESIS

**THE MYSTERY DEEPENS:**
1. UpdateCamera DrawCallback fires every frame ✓
2. The shader unconditionally outputs CYAN
3. OpenGL texture ID is consistent (ID 25)
4. Terrain binds the same texture to unit 7
5. Yet terrain sees WHITE after frame 1

**Possible explanations:**
1. **FBO not actually rendering to the texture** - DrawCallback fires but FBO output goes elsewhere
2. **OpenMW's rendering pipeline has multiple passes** - terrain might be sampling before UpdateCamera renders on frame 2+
3. **Shader not actually being used** - OSG might not be applying the program to the quad
4. **Different GL context** - texture might exist in different contexts with different contents
5. **OSG internal state issue** - FBO attachment might be getting detached/reattached incorrectly

---

## CURRENT DEBUG STATE

### snowdeformation.cpp
- Depth camera clears to MAGENTA
- Cull callback DISABLED
- Has `DepthCameraDrawCallback` logging

### snowdeformationupdater.cpp
- Binds `getDeformationMap()` to terrain unit 7
- Logs GL texture ID every 60 frames

### snowsimulation.cpp
- debugMode = 1 (output CYAN)
- Ping-pong DISABLED (always buffer 0)
- Clear mask DISABLED (`setClearMask(0)`)
- Sampler uniforms FIXED (use simple int constructor)
- Texture format: GL_RGBA / GL_UNSIGNED_BYTE (simplified)
- No initial image set on textures
- `SnowCameraDrawCallback` on all cameras with GL texture ID logging

### snowsimulation.hpp
- `getOutputTexture()` returns `mAccumulationMap[0]` directly (bypass blur)

### snow_update.frag
- **HARD-CODED to output CYAN unconditionally** (line 16-17)

---

## NEXT STEPS TO INVESTIGATE

### Step 1: Verify FBO Attachment is Correct on Frame 2+
Add logging inside DrawCallback to check FBO status:
```cpp
virtual void operator()(osg::RenderInfo& renderInfo) const override
{
    // Check what's currently bound as GL_DRAW_FRAMEBUFFER
    GLint currentFBO = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &currentFBO);
    Log(Debug::Info) << "[SnowSim] UpdateCamera: Current FBO=" << currentFBO;

    // Check FBO attachment
    GLint attachedTex = 0;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attachedTex);
    Log(Debug::Info) << "[SnowSim] UpdateCamera: FBO Color0 attached tex=" << attachedTex;
}
```

### Step 2: Check Render Order / Timing
The terrain might be rendered BEFORE the UpdateCamera on frame 2+. Check if:
- PRE_RENDER cameras run before or after terrain cull/draw
- OpenMW has a specific frame sequencing that differs from standard OSG

### Step 3: Use glReadPixels to Verify Texture Contents
After UpdateCamera renders, read back the texture contents:
```cpp
// In UpdateCamera DrawCallback (after render)
glBindTexture(GL_TEXTURE_2D, glTexID);
unsigned char pixels[4];
glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
Log(Debug::Info) << "Texture[0,0] = " << (int)pixels[0] << "," << (int)pixels[1] << "," << (int)pixels[2];
```
Expected: R=0, G=255, B=255 (CYAN)
If WHITE: R=255, G=255, B=255 means FBO isn't writing

### Step 4: Check OSG's RenderStage for the Camera
OSG might be skipping the actual render for some reason. Check:
- Camera's RenderStage has valid draw commands
- The quad geometry is in the render graph

### Step 5: Look at OpenMW Water/Shadow RTT Implementation
These systems successfully use RTT in OpenMW. Compare:
- How they set up their cameras
- How they attach textures
- Where they add cameras to the scene graph
- Any special flags or modes they use

### Step 6: Try PIXEL_BUFFER_RTT Instead of FBO
Some drivers have FBO issues:
```cpp
mUpdateCamera->setRenderTargetImplementation(osg::Camera::PIXEL_BUFFER_RTT);
```

### Step 7: Explicitly Call dirtyBound() / dirtyGLObjects()
Force OSG to re-realize the texture:
```cpp
mAccumulationMap[0]->dirtyTextureObject();
```

---

## DATA FLOW DIAGRAM

```
[Depth Camera] ──renders──> [mObjectMaskMap]
      │                            │
      │ PRE_RENDER 0              │ texture unit 1
      ▼                            ▼
[Update Camera] ──reads──> objectMask sampler
      │
      │ PRE_RENDER 1       reads previousFrame from unit 0
      │                     writes to mAccumulationMap[0] (no ping-pong)
      │
      │ DrawCallback FIRES every frame ✓
      │ Shader outputs CYAN unconditionally
      ▼
[mAccumulationMap[0]] (GL TexID: 25)
      │
      │ (blur bypassed for debug)
      │
      │ texture unit 7
      ▼
[Terrain Shader] ──reads──> snowDeformationMap
      │
      │ Same GL TexID: 25 ✓
      ▼
Frame 1: CYAN ✓
Frame 2+: WHITE ???
```

**THE MYSTERY:**
```
Frame 1:
  1. UpdateCamera renders CYAN to GL texture 25 ✓
  2. Terrain samples GL texture 25 → CYAN ✓

Frame 2+:
  1. UpdateCamera DrawCallback fires ✓
  2. Shader should output CYAN (unconditional)
  3. GL texture 25 should contain CYAN
  4. Terrain samples GL texture 25 → WHITE ???

  SOMETHING IS WRONG BETWEEN STEPS 2 AND 4:
  - Either FBO doesn't actually write to texture 25
  - Or terrain samples before UpdateCamera renders
  - Or there's a GL state/sync issue
```

---

## KEY FILES

| File | Purpose |
|------|---------|
| `components/terrain/snowdeformation.cpp` | Manager, depth camera setup |
| `components/terrain/snowsimulation.cpp` | RTT pipeline, update/blur cameras |
| `components/terrain/snowdeformationupdater.cpp` | Binds texture to terrain |
| `files/shaders/compatibility/snow_update.frag` | Accumulation shader |
| `files/shaders/compatibility/terrain.vert` | Reads deformation, displaces vertices |
| `files/shaders/compatibility/terrain.frag` | Debug visualization (lines 67-74) |

---

## TO RESTORE NORMAL OPERATION

After fixing, revert these changes:

1. **snowdeformation.cpp:**
```cpp
mDepthCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f)); // BLACK
mDepthCamera->setCullMask((1 << 3) | (1 << 4) | (1 << 10)); // Uncomment
mDepthCamera->setCullCallback(new DepthCameraCullCallback(...)); // Uncomment
// Remove DepthCameraDrawCallback
```

2. **snowsimulation.cpp:**
```cpp
mUpdateCamera->setClearMask(GL_COLOR_BUFFER_BIT); // Re-enable clear
ss->addUniform(new osg::Uniform("debugMode", 0)); // Normal operation
// Re-enable ping-pong buffer swapping
// Remove debug logging
// Remove SnowCameraDrawCallback from all cameras
// Restore GL_RGBA16F_ARB format
```

3. **snowsimulation.hpp:**
```cpp
osg::Texture2D* getOutputTexture() const { return mBlurredDeformationMap.get(); }
```

4. **snowdeformationupdater.cpp:**
```cpp
// Remove GL texture ID logging
```

5. **snow_update.frag:**
```glsl
// Remove the hard-coded CYAN output at lines 14-17
```

---

## RELATED OPENMW RTT EXAMPLES

Look at how these systems implement RTT for reference:
- **Water reflection/refraction cameras** - `apps/openmw/mwrender/water.cpp`
- **Shadow map cameras** - `components/sceneutil/shadow.cpp`
- **LocalMap RTT** - `apps/openmw/mwrender/localmap.cpp`

These might show the correct pattern for persistent RTT textures in OpenMW's architecture.

---

## SUMMARY OF WHAT WE KNOW

1. **Camera executes** - DrawCallback proves UpdateCamera runs every frame
2. **Shader should output CYAN** - Unconditional output, no logic involved
3. **Same texture** - Both OSG pointer and GL ID match between writer and reader
4. **Frame 1 works** - Proves the entire pipeline CAN work
5. **Frame 2+ fails** - Something changes between frame 1 and 2

**MOST LIKELY CAUSE:**
The FBO attachment or GL state is different on frame 2+. Either:
- OSG's FBO is pointing somewhere else
- The texture gets recreated with a new GL ID internally
- Render order is different on frame 2+
- There's a GL sync/flush issue

---

## SESSION: Ripples-Style Implementation Attempt (2024-12-01)

### Hypothesis
The OSG camera-based RTT mechanism isn't reliably rendering to the FBO after frame 1. OpenMW's water ripples system works because it uses `drawImplementation()` to manually control FBO binding rather than relying on OSG's camera RTT.

### Changes Made

#### 1. Complete Rewrite of SnowSimulation (Ripples Pattern)
Rewrote `snowsimulation.cpp` and `snowsimulation.hpp` to follow the `RipplesSurface` pattern:

- **Created `SnowSimulationSurface` class** extending `osg::Geometry` (like `RipplesSurface`)
- **Added critical VBO settings:**
  ```cpp
  setUseDisplayList(false);
  setUseVertexBufferObjects(true);
  ```
- **Uses fullscreen triangle** (more efficient than quad, same as ripples)
- **Manual FBO control in `drawImplementation()`:**
  ```cpp
  mFBOs[writeBuffer]->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
  osg::Geometry::drawImplementation(renderInfo);
  ```
- **Double-buffered state** for thread safety (like ripples)
- **Ping-pong texture management** internally

#### 2. Fixed snow_update.frag
- Removed the unconditional CYAN debug output
- Restored normal accumulation logic

#### 3. Fixed blur shaders
- Changed output from `vec4(result, 0.0, 0.0, 1.0)` to `vec4(result, result, result, 1.0)`
- Ensures terrain shader can read `.r` channel correctly

#### 4. Restored snowdeformation.cpp
- Depth camera clears to BLACK (was MAGENTA for debug)
- Re-enabled cull callback for rendering actors
- Disabled debug overlay temporarily

#### 5. Added FBO unbinding
- After drawing, explicitly unbind FBO to prevent subsequent renders going to wrong target:
  ```cpp
  ext->glBindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
  ```

### Test Results

**Result: GAME FREEZES on first frame**

Log output before freeze:
```
[SnowSim] drawImplementation ENTER frame 1
[SnowSim] Step 1: Got frame stamp
[SnowSim] Step 2: Buffers read=0 write=1
[SnowSim] Step 3: Textures ensured
[SnowSim] Step 4: StateSet applied
[SnowSim] Step 5: Program applied
[SnowSim] Step 6: Uniforms applied
[SnowSim] Step 7: Input textures bound
[SnowSim] Step 8: FBO applied, about to draw...
[SnowSim] Step 9: Draw complete!
[SnowSim] Step 10: FBO unbound
[SnowSim] drawImplementation EXIT frame 1
```

The `drawImplementation` **completes successfully** on frame 1, then the game freezes. The freeze happens AFTER the simulation runs but BEFORE frame 2 starts.

### Analysis

1. **drawImplementation works** - All steps complete, FBO is bound and unbound
2. **Freeze is NOT in the simulation** - Logs show clean exit from drawImplementation
3. **Freeze happens after** - No frame 2 log entries appear

Possible causes:
- Terrain shader infinite loop when sampling deformation map
- OSG state corruption after manual FBO manipulation
- Something in the depth camera cull callback causing infinite traversal
- Debug overlay (now disabled) was causing issues

### Current State of Files

| File | Status |
|------|--------|
| `snowsimulation.cpp` | Rewritten with Ripples pattern, extensive debug logging |
| `snowsimulation.hpp` | New class structure with SnowSimulationSurface |
| `snowdeformation.cpp` | Debug overlay disabled, depth camera restored |
| `snow_update.frag` | Normal operation (debug removed) |
| `blur_*.frag` | Fixed RGB output |

### Next Steps to Try

1. **Disable terrain deformation sampling** - Comment out texture2D calls in terrain.vert/frag to see if that's where it freezes
2. **Check depth camera cull callback** - It iterates root children, might cause infinite loop
3. **Test with ripples disabled** - See if ripples + snow simulation conflict
4. **Add logging AFTER drawImplementation exits** - Confirm where exactly freeze occurs
5. **Try without the wrapper Camera** - Just add SnowSimulationSurface directly to scene (no PRE_RENDER camera)

### Code Diff Summary

```
snowsimulation.cpp            | 574 lines changed (complete rewrite)
snowsimulation.hpp            | 128 lines changed (new class structure)
snowdeformation.cpp           |  65 lines changed (debug overlay disabled)
snow_update.frag              |  66 lines changed (debug removed)
blur_horizontal.frag          |   2 lines changed (RGB output)
blur_vertical.frag            |   2 lines changed (RGB output)
```

---

## SESSION: Camera-Based RTT Rewrite (2024-12-01 evening)

### Problem After Ripples-Style Attempt
The game froze completely when using the `drawImplementation()` approach. Reverted to simpler camera-based RTT.

### Changes Made

#### 1. Reverted to osg::Group-based SnowSimulation
- No more `osg::Geometry` base class with `drawImplementation()`
- Uses standard OSG camera RTT with `attach()`
- Three child cameras: UpdateCamera, BlurHCamera, BlurVCamera

#### 2. Fixed Ping-Pong Buffer Logic
Previous logic was:
```cpp
int readIndex = (mWriteBufferIndex + 1) % 2;  // WRONG on first frame
int writeIndex = mWriteBufferIndex;
// No swap happening!
```

New logic:
```cpp
int readIndex = mWriteBufferIndex;  // What we wrote last frame
mWriteBufferIndex = (mWriteBufferIndex + 1) % 2;  // Swap
int writeIndex = mWriteBufferIndex;  // What we'll write this frame
```

#### 3. Added Texture Initialization
Accumulation textures were uninitialized (garbage data). Added:
```cpp
osg::ref_ptr<osg::Image> blackImage = new osg::Image;
blackImage->allocateImage(2048, 2048, 1, GL_RGBA, GL_FLOAT);
memset(blackImage->data(), 0, blackImage->getTotalSizeInBytes());
mAccumulationMap[i]->setImage(blackImage);
```

### Test Results

**Result: Still WHITE everywhere**

The terrain is uniformly pushed down (white = 1.0 deformation) across the entire RTT area.

### Analysis

**Possible causes of all-white:**

1. **Object mask is all white** - Depth camera rendering everything white, not just actors
2. **Shader not reading object mask correctly** - Texture unit binding issue
3. **Accumulation runaway** - previousFrame feedback loop filling to white
4. **Wrong texture being sampled** - Terrain sampling wrong buffer

### Current Debug State

**debugMode = 2** set in snowsimulation.cpp to show ONLY the object mask.

If result is:
- **WHITE**: Object mask has problem (depth camera rendering everything)
- **BLACK with WHITE actor shapes**: Object mask is correct, problem is in accumulation
- **STILL WHITE**: Update shader not receiving correct objectMask texture

### Next Steps

1. **Test with debugMode=2** - See what object mask contains
2. **Check depth camera cull mask** - Might be rendering terrain too
3. **Check blur pass render order** - PRE_RENDER priorities might conflict
4. **Bypass blur entirely** - Return accumulation buffer directly

### Files Currently Modified

| File | Status |
|------|--------|
| `snowsimulation.cpp` | Camera-based RTT, ping-pong fixed, texture init added, debugMode=2 |
| `snowsimulation.hpp` | osg::Group based, simple interface |
| `snowdeformation.cpp` | Standard setup, cull callback enabled |
| `snow_update.frag` | debugMode support (0-4) |

### Data Flow (Current State)

```
[Depth Camera PRE_RENDER 0]
   - Cull mask: Actor(3) | Player(4) | Object(10)
   - Cull callback: Traverses root, skips Terrain/Sky/Water/Cameras
   - Override shader: Output white
   - Output: mObjectMaskMap

[Update Camera PRE_RENDER 1]
   - Input unit 0: mAccumulationMap[readIndex] (previous frame)
   - Input unit 1: mObjectMaskMap (actors)
   - debugMode=2: Output objectMask only (bypass accumulation)
   - Output: mAccumulationMap[writeIndex]

[Blur H Camera PRE_RENDER 3]
   - Input: mAccumulationMap[writeIndex]
   - Output: mBlurTempBuffer

[Blur V Camera PRE_RENDER 4]
   - Input: mBlurTempBuffer
   - Output: mBlurredDeformationMap (final)

[Terrain Rendering]
   - Samples: mBlurredDeformationMap on unit 7
   - If debugMode=2 working: Should show black with white actor shapes
```

---

## SESSION: Systematic RTT Pipeline Debug (2024-12-01 late)

### New Debug Plan Created
Created `SNOW_RTT_DEBUG_PLAN.md` with systematic bypass tests to isolate the problem.

### Test Results

| Test | Setup | Result | Conclusion |
|------|-------|--------|------------|
| **TEST 1** | Bypass entire simulation - bind ObjectMaskMap directly to terrain | **PASS** - Deformation only where actors are | Depth camera works correctly. Problem is in simulation pipeline |
| **TEST 2** | Bypass blur - bind AccumulationMap directly | **FAIL** - Deformation everywhere | Problem is in update shader or buffer management, NOT blur |
| **TEST 3** | Simplify update shader - pass through objectMask only | **FAIL** - Deformation everywhere | Problem is how objectMask is bound to update shader |
| **TEST 5** | Hardcode BLACK output in update shader | **FAIL** - Deformation everywhere | **Update shader output is NOT being read by terrain** |

### Critical Finding

**TEST 5 proves the terrain is NOT reading from the update shader's output.**

When snow_update.frag unconditionally outputs `vec4(0.0, 0.0, 0.0, 1.0)` (black), the terrain should show NO deformation. But deformation is still everywhere.

This means `getAccumulationMap()` returns a texture that the update shader is NOT rendering to.

### Root Cause Identified

**Ping-pong buffer mismatch:**

```cpp
// In getAccumulationMap():
return mAccumulationMap[0].get();  // ALWAYS returns buffer 0

// In update():
mWriteBufferIndex = (mWriteBufferIndex + 1) % 2;  // Alternates 0→1→0→1...
mUpdateCamera->attach(..., mAccumulationMap[writeIndex]);  // Writes to 0 or 1
```

The terrain was reading from buffer 0, but the update camera alternates between writing to buffer 0 and buffer 1. On odd frames, the terrain reads stale/uninitialized data from buffer 0 while the shader writes to buffer 1.

### Fix Applied

Changed `getAccumulationMap()` to return the current write buffer:

```cpp
// Before (WRONG):
osg::Texture2D* getAccumulationMap() const { return mAccumulationMap[0].get(); }

// After (CORRECT):
osg::Texture2D* getAccumulationMap() const { return mAccumulationMap[mWriteBufferIndex].get(); }
```

### Current State of Files

| File | Status |
|------|--------|
| `snowsimulation.hpp` | `getAccumulationMap()` returns `mAccumulationMap[mWriteBufferIndex]` |
| `snowsimulation.cpp` | Standard ping-pong logic |
| `snowdeformationupdater.cpp` | TEST 2 mode - binds `getAccumulationMap()` directly (bypass blur) |
| `snow_update.frag` | TEST 5 mode - outputs BLACK unconditionally |

### Next Steps

1. **Rebuild and test** with the fix applied
2. If TEST 5 passes (no deformation with black shader), restore normal shader logic
3. Re-enable blur passes
4. Test full pipeline with accumulation

### Files Modified This Session

```
components/terrain/snowsimulation.hpp    - getAccumulationMap() fix
components/terrain/snowdeformationupdater.cpp - TEST 2 bypass
files/shaders/compatibility/snow_update.frag - TEST 5 black output
SNOW_RTT_DEBUG_PLAN.md - Created systematic test plan
```

---

## SESSION: NodeMask and Vertex Shader Fix Attempt (2024-12-01 night)

### Hypothesis
The update camera might not be rendering because:
1. Missing `Mask_RenderToTexture` node mask (required by OpenMW)
2. Vertex shader not transforming vertices correctly

### Changes Made

#### 1. Added Mask_RenderToTexture to All Cameras
OpenMW filters cameras by node mask. Added `setNodeMask(1 << 17)` to:
- mUpdateCamera
- mBlurHCamera
- mBlurVCamera

Also added `setImplicitBufferAttachmentMask(0, 0)` to prevent automatic depth buffer creation.

#### 2. Fixed Vertex Shader
Changed `snow_update.vert` from:
```glsl
gl_Position = gl_Vertex;  // WRONG - doesn't apply projection
```
To:
```glsl
gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;  // Correct
```

#### 3. Fixed Sampler Uniform Constructors
Changed from array constructor (which causes warnings):
```cpp
new osg::Uniform(osg::Uniform::SAMPLER_2D, "name", 0)
```
To simple int constructor:
```cpp
new osg::Uniform("name", 0)
```

#### 4. Matched Texture Format to ObjectMaskMap
Changed AccumulationMap from `GL_RGBA16F_ARB/GL_FLOAT` to `GL_RGBA/GL_UNSIGNED_BYTE` to match ObjectMaskMap (which works).

#### 5. Disabled Ping-Pong
- Removed buffer swapping in `update()`
- `getAccumulationMap()` always returns buffer 0

### Test Results

| Test | Setup | Result |
|------|-------|--------|
| TEST 1 | ObjectMaskMap direct to terrain | **PASS** - Deformation only where actors stand |
| TEST 2 with fixes | AccumulationMap to terrain (50% red shader) | **FAIL** - Full deformation everywhere |
| TEST 2 + NodeMask | Added Mask_RenderToTexture (1<<17) | **NEW RESULT: NO deformation at all** |

### Analysis

**Adding NodeMask BROKE the system completely** - terrain now shows NO deformation.

This is actually interesting because it means:
1. Before NodeMask: Terrain was reading SOMETHING (white/full deformation)
2. After NodeMask: Terrain is reading BLACK (0 deformation)

Possible interpretations:
1. The camera IS now rendering (outputting 0.5 red → 50% deformation expected)
2. But terrain shows 0 deformation → still not reading the right texture
3. OR the NodeMask caused the camera to be skipped entirely
4. OR the terrain is now reading from an uninitialized/black texture

### Current State of Files

| File | Current State |
|------|---------------|
| `snowsimulation.cpp` | NodeMask(1<<17), single buffer, GL_RGBA format, fixed uniforms |
| `snowsimulation.hpp` | `getAccumulationMap()` returns buffer 0 |
| `snowdeformationupdater.cpp` | TEST 2 - binds AccumulationMap to unit 7 |
| `snow_update.frag` | Outputs solid `vec4(0.5, 0.0, 0.0, 1.0)` |
| `snow_update.vert` | Uses `gl_ModelViewProjectionMatrix * gl_Vertex` |

### Key Observation

The transition from "full deformation" to "no deformation" after adding NodeMask suggests:

**Before NodeMask:**
- Camera might have been rendering to default framebuffer (not the texture)
- Terrain was reading uninitialized texture data (garbage → interpreted as white)

**After NodeMask:**
- Camera might now be culled/skipped
- OR camera renders but output goes nowhere
- Terrain reads same uninitialized texture (now happens to be black due to removed setImage)

### Next Steps to Try

1. **Check if camera is being culled** - Add logging to verify camera traversal
2. **Try without NodeMask but WITH vertex shader fix** - Isolate which change caused the behavior change
3. **Add back texture initialization** - Ensure buffer 0 starts with known data
4. **Check OpenMW cull visitor** - See if Mask_RenderToTexture cameras need special handling
5. **Compare with Ripples camera setup** - Ripples uses setNodeMask AND works

### Data Flow After Changes

```
[Update Camera PRE_RENDER 1]
   - NodeMask: 1 << 17 (Mask_RenderToTexture)
   - Viewport: 2048x2048
   - FBO attached to mAccumulationMap[0]
   - Shader outputs vec4(0.5, 0, 0, 1)
   - Result: ??? (camera may be culled or output lost)

[Terrain]
   - Reads mAccumulationMap[0] on unit 7
   - Sees: 0 (no deformation)
   - Expected: 0.5 (50% deformation)
```

### Questions to Answer

1. Is the UpdateCamera actually being traversed/rendered after adding NodeMask?
2. Is the FBO attachment still valid after adding setImplicitBufferAttachmentMask(0,0)?
3. Does OpenMW's cull visitor have special handling for Mask_RenderToTexture cameras?
4. Are the vertices being rendered in the correct location (0,0 to 1,1 in ortho space)?
