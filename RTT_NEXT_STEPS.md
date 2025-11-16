# RTT Camera - Next Debugging Steps

**Date**: 2025-11-16
**Status**: Quad still not rendering (Max depth = 0)
**Current Issue**: Despite all setup appearing correct, the RTT camera is not rendering the footprint quad to the texture.

---

## Problem Summary

The RTT (Render-To-Texture) camera system has been extensively debugged and rewritten, but the quad still doesn't render. The symptom is:

```
[SNOW DIAGNOSTIC] Max depth in texture: 0
```

Expected: `Max depth in texture: 1.0` (from solid red test shader)

---

## What We've Tried (All Failed)

### 1. ✗ Coordinate System Fixes
- **Tried**: Changed from world/camera space confusion to pure camera-local quads
- **Result**: Max depth still 0

### 2. ✗ Projection Matrix Fixes
- **Tried**: Changed near/far from ±10000 to ±10
- **Result**: Max depth still 0

### 3. ✗ Quad Z Position Variations
- **Tried**: Z=-5, Z=0, multiple positions
- **Result**: Max depth still 0 for all positions

### 4. ✗ Timing Fixes
- **Tried**: Fixed node mask enable/disable timing (removed premature disabling)
- **Result**: Max depth still 0

### 5. ✗ Backface Culling
- **Tried**: Disabled culling with `GL_CULL_FACE OFF`
- **Result**: Max depth still 0

### 6. ✗ Depth Testing
- **Tried**: Disabled depth test with `GL_DEPTH_TEST OFF`
- **Result**: Max depth still 0

### 7. ✗ Added Normals
- **Tried**: Added normal array to quad geometry
- **Result**: Max depth still 0

### 8. ✗ Simplified Shader
- **Tried**: Using minimal test shader that just outputs `vec4(1.0, 0.0, 0.0, 1.0)`
- **Result**: Max depth still 0 (shader not running OR quad not visible)

---

## Current State of Code

### File: `components/terrain/snowdeformation.cpp`

**RTT Camera Setup** (lines 93-162):
- Reference frame: `ABSOLUTE_RF` ✓
- Render order: `PRE_RENDER` ✓
- Projection: Orthographic, -300 to +300 X/Y, -10 to +10 Z ✓
- Clear: Enabled, black color ✓
- Viewport: Matches texture resolution ✓

**Footprint Quad Setup** (lines 183-343):
- Vertices: At Z=0 in camera-local space ✓
- UVs: Standard 0-1 mapping ✓
- Normals: +Z direction ✓
- Primitive: GL_QUADS, 4 vertices ✓
- State: Depth test OFF, culling OFF ✓
- Shader: Simple test shader (outputs solid red) ✓

**View Matrix** (lines 504-552):
- Eye: 100 units above player
- Center: At player position
- Up: -Y (South)
- Updates each frame ✓

**Node Mask Management** (lines 633-642):
- Camera enabled when stamping ✓
- Footprint group enabled ✓
- Other groups disabled ✓
- No premature disabling ✓

**Diagnostic Logs** (lines 632-662):
```
[SNOW DIAGNOSTIC] Projection matrix valid: 1
[SNOW DIAGNOSTIC] View matrix valid: 1
[SNOW DIAGNOSTIC] Camera reference frame: 1 (ABSOLUTE_RF)
[SNOW DIAGNOSTIC] Camera render order: 0 (PRE_RENDER)
[SNOW DIAGNOSTIC] Camera clear mask: 257
[SNOW DIAGNOSTIC] RTT Camera node mask: 4294967295 (enabled)
[SNOW DIAGNOSTIC] Footprint group node mask: 4294967295 (enabled)
[SNOW DIAGNOSTIC] Current texture attached: 1
```

All diagnostics show correct setup, but **Max depth = 0** consistently.

---

## Possible Remaining Causes

### 1. OSG State Inheritance Issue
**Theory**: The camera might be inheriting render state from the parent scene that's preventing rendering.

**Test**:
- Try setting the camera's state set to explicitly override all inherited state
- Add `mRTTCamera->setStateSet()` with a fresh, minimal state set
- Use `osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED`

**Code to try**:
```cpp
osg::ref_ptr<osg::StateSet> cameraState = new osg::StateSet;
cameraState->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
cameraState->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
cameraState->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
mRTTCamera->setStateSet(cameraState);
```

### 2. Shader Compilation Failure
**Theory**: The shader might not be compiling, but we're not checking for errors.

**Test**:
- Add shader compilation error checking
- Use `program->addBindAttribLocation()` to ensure vertex attributes are bound
- Try using a built-in OSG shader instead of custom GLSL

**Code to try**:
```cpp
// After creating program
osg::ref_ptr<osg::Shader::PerContextCompiledObjects> compiled = vertShader->getCompiledObjects();
if (compiled) {
    // Check compilation status
}

// Or use OSG's built-in shader manager
mSceneManager->getShaderManager().getProgram("simple");
```

### 3. Texture Format Mismatch
**Theory**: The texture internal format might not match what the shader expects.

**Current format**: `GL_RGBA16F` (float, HDR)
**Shader output**: `vec4(1.0, 0.0, 0.0, 1.0)` (should work with any format)

**Test**:
- Try changing texture format to `GL_RGBA8` (unsigned byte)
- Change shader to output lower values (0.5 instead of 1.0)

**Code to try**:
```cpp
// In createDeformationTextures()
mDeformationTexture[i]->setInternalFormat(GL_RGBA8);
// And update shader to:
gl_FragColor = vec4(0.5, 0.0, 0.0, 1.0);
```

### 4. FBO Attachment Not Complete
**Theory**: The framebuffer might not be complete when rendering.

**Test**:
- Add FBO completeness check using a draw callback
- Try attaching a depth buffer explicitly
- Check if `attach()` is actually working

**Code to try**:
```cpp
// Add depth buffer attachment
osg::ref_ptr<osg::Texture2D> depthTex = new osg::Texture2D;
depthTex->setTextureSize(mTextureResolution, mTextureResolution);
depthTex->setInternalFormat(GL_DEPTH_COMPONENT);
mRTTCamera->attach(osg::Camera::DEPTH_BUFFER, depthTex.get());

// Or in a draw callback, check FBO status:
GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
if (status != GL_FRAMEBUFFER_COMPLETE) {
    Log(Debug::Error) << "FBO not complete: " << status;
}
```

### 5. Camera Not Actually Rendering
**Theory**: OSG might be culling the camera or not traversing it.

**Test**:
- Add a custom cull callback to the camera to log when culling happens
- Add a custom draw callback to log when drawing happens
- Check if the camera's stats show any draw calls

**Code to try**:
```cpp
struct DiagnosticCullCallback : public osg::NodeCallback
{
    virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        Log(Debug::Info) << "[SNOW DIAGNOSTIC] Camera cull callback executed";
        traverse(node, nv);
    }
};

struct DiagnosticDrawCallback : public osg::Camera::DrawCallback
{
    virtual void operator()(osg::RenderInfo& renderInfo) const
    {
        Log(Debug::Info) << "[SNOW DIAGNOSTIC] Camera draw callback executed";
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        Log(Debug::Info) << "[SNOW DIAGNOSTIC] FBO status: " << status;
    }
};

mRTTCamera->setCullCallback(new DiagnosticCullCallback);
mRTTCamera->setInitialDrawCallback(new DiagnosticDrawCallback);
```

### 6. Clear Happening After Geometry Render
**Theory**: The clear operation might be happening in the wrong order.

**Test**:
- Try disabling clear temporarily
- Try using `POST_RENDER` instead of `PRE_RENDER`
- Try rendering to the texture manually without RTT camera

**Code to try**:
```cpp
// Temporarily disable clear
mRTTCamera->setClearMask(0);

// Or change render order
mRTTCamera->setRenderOrder(osg::Camera::POST_RENDER);
```

### 7. Orthographic Projection Issue
**Theory**: The orthographic projection might have an issue with how OSG interprets near/far.

**Test**:
- Try using a perspective projection temporarily
- Try explicitly setting the projection matrix manually
- Try rendering with an identity view/projection to rule out transform issues

**Code to try**:
```cpp
// Try identity transforms
mRTTCamera->setViewMatrix(osg::Matrix::identity());
mRTTCamera->setProjectionMatrix(osg::Matrix::ortho2D(-300, 300, -300, 300));

// Or try perspective
mRTTCamera->setProjectionMatrixAsPerspective(90.0, 1.0, 1.0, 1000.0);
```

---

## Recommended Testing Order

### Test 1: Add Draw Callback Diagnostics
This will tell us if the camera is actually executing draw commands.

**Priority**: HIGH
**Effort**: Low
**Location**: `setupRTT()` after camera creation

### Test 2: Disable Clear
If clearing is overwriting everything, disabling it should show accumulated red.

**Priority**: HIGH
**Effort**: Low
**Location**: `setupRTT()` line 144

### Test 3: Check FBO Completeness
Ensure the framebuffer is valid.

**Priority**: HIGH
**Effort**: Medium
**Location**: Add to draw callback

### Test 4: Override Inherited State
Clear any state that might be preventing rendering.

**Priority**: MEDIUM
**Effort**: Medium
**Location**: `setupRTT()` after camera creation

### Test 5: Change Texture Format
Rule out float format issues.

**Priority**: MEDIUM
**Effort**: Low
**Location**: `createDeformationTextures()`

### Test 6: Shader Compilation Check
Ensure shader is actually compiling and running.

**Priority**: MEDIUM
**Effort**: Medium
**Location**: `setupFootprintStamping()` after program creation

### Test 7: Try Different Render Order
Rule out timing issues.

**Priority**: LOW
**Effort**: Low
**Location**: `setupRTT()` line 110

---

## Expected Diagnostic Output

If the camera is rendering correctly, you should see:

```
[SNOW DIAGNOSTIC] Camera cull callback executed
[SNOW DIAGNOSTIC] Camera draw callback executed
[SNOW DIAGNOSTIC] FBO status: 36053 (GL_FRAMEBUFFER_COMPLETE)
[SNOW DIAGNOSTIC] Initial draw callback - before render
[SNOW DIAGNOSTIC] Final draw callback - after render
[SNOW DIAGNOSTIC] Texture data read from GPU
[SNOW DIAGNOSTIC] Max depth in texture: 1.0 ← SUCCESS!
```

If you see:
- **No cull callback**: Camera not being traversed
- **No draw callback**: Camera not executing draw phase
- **FBO status != 36053**: Framebuffer not complete
- **Max depth = 0**: Quad not rendering or shader not running

---

## Alternative Approaches to Consider

If none of the above works, consider:

1. **Use osg::Image as render target directly**
   - Instead of rendering to texture, attach an osg::Image
   - OSG will handle FBO setup automatically

2. **Use a simpler test case**
   - Create a minimal OSG application that ONLY does RTT
   - If that works, compare to this implementation
   - If that fails, might be an OSG version or driver issue

3. **Check OpenMW's existing RTT usage**
   - Look for other parts of OpenMW that use RTT cameras
   - Copy their setup exactly
   - Files to check: water reflections, shadow maps, post-processing

4. **Try CPU-side rendering as fallback**
   - Render footprints to an osg::Image on CPU
   - Upload to texture each frame
   - Slower but guaranteed to work

---

## Files to Review

1. **components/terrain/snowdeformation.cpp**
   - Main implementation file
   - Lines 93-162: RTT camera setup
   - Lines 183-343: Footprint quad setup
   - Lines 554-673: Footprint stamping logic

2. **components/terrain/snowdeformation.hpp**
   - Class definition
   - Member variables for camera/textures/quads

3. **files/shaders/compatibility/terrain.vert** (recently opened by user)
   - Might need to check if terrain shader has any conflicts

---

## Quick Diagnostic Script

Add this to the top of `stampFootprint()` to dump all relevant state:

```cpp
static bool diagnosticDone = false;
if (!diagnosticDone && stampCount == 1) {
    diagnosticDone = true;

    Log(Debug::Info) << "=== FULL DIAGNOSTIC DUMP ===";
    Log(Debug::Info) << "Camera valid: " << (mRTTCamera.valid());
    Log(Debug::Info) << "Camera node mask: " << mRTTCamera->getNodeMask();
    Log(Debug::Info) << "Camera reference frame: " << mRTTCamera->getReferenceFrame();
    Log(Debug::Info) << "Camera render order: " << mRTTCamera->getRenderOrder();
    Log(Debug::Info) << "Camera num children: " << mRTTCamera->getNumChildren();

    Log(Debug::Info) << "Footprint group valid: " << (mFootprintGroup.valid());
    Log(Debug::Info) << "Footprint group node mask: " << mFootprintGroup->getNodeMask();
    Log(Debug::Info) << "Footprint group num children: " << mFootprintGroup->getNumChildren();

    Log(Debug::Info) << "Footprint quad valid: " << (mFootprintQuad.valid());
    Log(Debug::Info) << "Footprint quad num vertices: " << mFootprintQuad->getVertexArray()->getNumElements();
    Log(Debug::Info) << "Footprint quad num primitives: " << mFootprintQuad->getNumPrimitiveSets();

    osg::StateSet* ss = mFootprintQuad->getStateSet();
    Log(Debug::Info) << "Footprint quad state set valid: " << (ss != nullptr);
    if (ss) {
        osg::StateAttribute* prog = ss->getAttribute(osg::StateAttribute::PROGRAM);
        Log(Debug::Info) << "Shader program attached: " << (prog != nullptr);
    }

    Log(Debug::Info) << "Texture 0 valid: " << (mDeformationTexture[0].valid());
    Log(Debug::Info) << "Texture 1 valid: " << (mDeformationTexture[1].valid());
    Log(Debug::Info) << "Current texture index: " << mCurrentTextureIndex;

    auto attachments = mRTTCamera->getBufferAttachmentMap();
    Log(Debug::Info) << "Camera has " << attachments.size() << " buffer attachments";
}
```

---

## Summary

**What works**: Camera setup, texture creation, node mask management, timing, diagnostics
**What doesn't work**: Actual rendering to texture
**Next step**: Add draw callbacks and FBO completeness checks to find WHERE the rendering pipeline is failing

**Most likely issue**: Either FBO not complete, shader not compiling, or camera not executing draw traversal.

**Quick win test**: Disable clear (`setClearMask(0)`) and see if anything accumulates.

---

**Good luck!** 🍀
