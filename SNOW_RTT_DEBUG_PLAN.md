# Snow RTT Deformation - Systematic Debug Plan

## Problem Statement
The terrain is deformed across the entire RTT area instead of only where actors are standing.
The deformation value appears to be ~1.0 everywhere instead of 0.0 (no deformation) with actor silhouettes at 1.0.

## RTT Pipeline Overview
```
[Depth Camera] --> [ObjectMaskMap] --> [Update Shader] --> [AccumulationMap] --> [Blur H] --> [Blur V] --> [BlurredDeformationMap] --> [Terrain]
     PRE_RENDER 0                         PRE_RENDER 1                            PRE_RENDER 3   PRE_RENDER 4
```

## Test Strategy
Bypass parts of the pipeline to isolate where the corruption occurs.

---

## TEST 1: Bypass Everything - Use ObjectMaskMap Directly

**Goal:** Verify the depth camera produces correct output (black background, white actor silhouettes)

**Change:** In `snowdeformationupdater.cpp`, bind `mObjectMaskMap` instead of the simulation output:
```cpp
// In apply():
stateset->setTextureAttributeAndModes(7, manager->getObjectMaskMap(), osg::StateAttribute::ON);
```

**Expected Result:**
- Terrain deformed ONLY where actors are standing
- No deformation elsewhere (black = 0 in ObjectMask)

**If PASS:** Problem is in simulation pipeline (update shader, blur, or buffer management)
**If FAIL:** Problem is in depth camera setup

---

## TEST 2: Bypass Blur - Use AccumulationMap Directly

**Goal:** Check if blur passes are corrupting the signal

**Change:** In `snowdeformation.hpp`, modify `getDeformationMap()`:
```cpp
osg::Texture2D* getDeformationMap() const { return mSimulation ? mSimulation->getAccumulationMap() : nullptr; }
```

**Expected Result:**
- Same as Test 1 but with accumulation (trails should persist if working)

**If PASS:** Problem is in blur passes
**If FAIL:** Problem is in update shader or buffer swap logic

---

## TEST 3: Simplify Update Shader - Pass Through Object Mask Only

**Goal:** Check if accumulation/previousFrame logic is corrupting the signal

**Change:** In `snow_update.frag`, output only the object mask:
```glsl
void main()
{
    vec2 uv = gl_TexCoord[0].xy;
    float newValue = texture2D(objectMask, uv).r;
    gl_FragColor = vec4(newValue, 0.0, 0.0, 1.0);
}
```

**Expected Result:**
- Same as Test 1

**If PASS:** Problem is in accumulation logic (previousFrame sampling, decay, or max())
**If FAIL:** Problem is in how objectMask is bound to the update shader

---

## TEST 4: Check Ping-Pong Buffer State

**Goal:** Verify buffers are initialized correctly and swap logic works

**Change:** Add logging in `SnowSimulation::update()`:
```cpp
Log(Debug::Info) << "SnowSim: readIndex=" << readIndex << " writeIndex=" << writeIndex;
```

Also verify initial texture state by checking if `mAccumulationMap[i]` images are actually black.

---

## TEST 5: Hardcode Output in Update Shader

**Goal:** Verify the update shader is actually being used

**Change:** In `snow_update.frag`:
```glsl
gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); // Force black output
```

**Expected Result:**
- NO deformation anywhere (all black = 0)

**If terrain still deformed:** The shader isn't being applied, or terrain is reading from wrong texture

---

## Current Test Status

| Test | Status | Result | Conclusion |
|------|--------|--------|------------|
| 1    | PASS | Deformation only where actors are | Depth camera works. Problem is in simulation pipeline |
| 2    | FAIL | Deformation everywhere | Problem is in update shader or buffer swap, NOT blur |
| 3    | FAIL | Deformation everywhere | objectMask binding to update shader is NOT the issue |
| 4    | PENDING | - | - |
| 5    | FAIL | Deformation everywhere | **Terrain is NOT reading from update shader output** |

---

## Files to Modify for Each Test

- **Test 1:** `components/terrain/snowdeformationupdater.cpp` (line ~51)
- **Test 2:** `components/terrain/snowdeformation.hpp` (line ~85)
- **Test 3:** `files/shaders/compatibility/snow_update.frag`
- **Test 4:** `components/terrain/snowsimulation.cpp` (in update())
- **Test 5:** `files/shaders/compatibility/snow_update.frag`

---

## Critical Finding from TEST 5

**The update shader outputs BLACK unconditionally, yet terrain still shows deformation everywhere.**

This proves the terrain is NOT sampling from the texture that the update shader renders to.

### Attempted Fixes (All Failed)

1. **Return `mAccumulationMap[0]`** - Always returns buffer 0
   - Result: FAIL - Deformation everywhere

2. **Return `mAccumulationMap[mWriteBufferIndex]`** - Returns current write buffer
   - Result: FAIL - Deformation everywhere
   - Reason: mWriteBufferIndex points to buffer being written THIS frame, which hasn't rendered yet when terrain samples

3. **Return `mAccumulationMap[(mWriteBufferIndex + 1) % 2]`** - Returns LAST frame's write buffer (read buffer)
   - Result: FAIL - Deformation everywhere
   - This SHOULD have worked but didn't

### Remaining Hypotheses

1. **The update camera never actually renders to the accumulation textures**
   - Camera might be culled, not in scene graph, or not executing
   - FBO attachment might fail silently

2. **Texture initialization overwrites FBO output**
   - The `setImage()` with black data might be re-applied each frame
   - Or the image data persists and overwrites GPU-side FBO output

3. **Different OSG texture objects**
   - `getAccumulationMap()` might return a copy or different instance
   - Need to verify exact pointer addresses match

4. **Render order issue**
   - PRE_RENDER cameras might run AFTER terrain samples the texture
   - Need to verify frame timing

5. **The accumulation textures are never actually used by the FBO**
   - `attach()` might not properly link the texture to the camera's FBO

---

## Next Debug Steps

### Step A: Verify Camera is Rendering
Add a DrawCallback to mUpdateCamera to log when it actually renders:
```cpp
struct DebugDrawCallback : public osg::Camera::DrawCallback {
    virtual void operator()(osg::RenderInfo& ri) const override {
        Log(Debug::Info) << "[SnowSim] UpdateCamera DRAW executed!";
    }
};
mUpdateCamera->setFinalDrawCallback(new DebugDrawCallback());
```

### Step B: Verify FBO Attachment
Log the actual GL texture IDs to ensure they match:
```cpp
// In update():
GLuint texID = mAccumulationMap[writeIndex]->getTextureObject(0)->id();
Log(Debug::Info) << "[SnowSim] Writing to GL tex ID: " << texID;

// In getAccumulationMap():
// Also log what we're returning
```

### Step C: Remove Initial Image
The `setImage()` call might interfere with FBO rendering. Try removing it:
```cpp
// Comment out: mAccumulationMap[i]->setImage(clearImage);
```

### Step D: Test with Single Buffer (No Ping-Pong)
Simplify by disabling ping-pong entirely:
```cpp
// Always use buffer 0, never swap
mUpdateCamera->attach(osg::Camera::COLOR_BUFFER, mAccumulationMap[0]);
// In getAccumulationMap(): return mAccumulationMap[0];
// Don't swap mWriteBufferIndex
```

### Step E: Check if SnowSimulation is Even in Scene Graph
Verify the simulation node is actually added and traversed:
```cpp
// In SnowDeformationManager constructor or where simulation is created:
Log(Debug::Info) << "[SnowSim] Simulation node: " << mSimulation.get();
Log(Debug::Info) << "[SnowSim] Simulation parent count: " << mSimulation->getNumParents();
```

---

## Notes
- After each test, rebuild and run the game
- Observe: Is deformation only where actors stand, or everywhere in RTT area?
- Record results in the table above
