# The Final Challenge: Viewport Override Mystery

**Date**: 2025-11-16
**Priority**: 🔴 **CRITICAL BLOCKER**
**Status**: Investigating

---

## 🎯 The Core Problem

**The RTT camera viewport is being overridden during rendering, and we cannot prevent it.**

### Timeline of Events (Every Frame):
```
1. Setup: Camera configured with 1024x1024 viewport         ✓
2. Protected: Viewport added to StateSet with PROTECTED flag ✓
3. Initial Callback: Viewport is 1280x720 (main window)     ✗
4. We Force: Changed to 1024x1024 via state->applyAttribute() ✓
5. Render: Quad geometry rendered (presumably)              ?
6. Final Callback: Viewport is 800x600 (!!)                 ✗✗
```

**Result**: Quad renders with WRONG viewport (if it renders at all) → black texture → max depth = 0

---

## 🔬 What We've Discovered

### ✅ What's Working:
- FBO is COMPLETE (verified in callback)
- Camera is being traversed (cull callback executes)
- Camera is rendering (initial + final callbacks execute)
- 2 buffer attachments (color + depth)
- Can successfully force viewport to 1024x1024 in initial callback
- Viewport protection is applied via StateSet

### ❌ What's NOT Working:
- **Viewport doesn't stay at 1024x1024**
- Changes from 1024x1024 → 800x600 during render
- PROTECTED flag has no effect
- Saved images are pitch black
- Max depth = 0
- No terrain deformation visible

### 🤔 The 800x600 Mystery:
This viewport size is mysterious:
- **Not** 1280x720 (main window size)
- **Not** 1024x1024 (texture size)
- **Not** any known OpenMW viewport
- **Appears consistently** in final callback

**Hypothesis**: This might be:
1. A default/fallback viewport size
2. An internal OSG rendering viewport
3. A viewport from another render stage
4. A hardcoded value somewhere in OpenMW

---

## 🧩 Attempted Fixes (All Failed)

### Fix #1: Set viewport on camera ❌
```cpp
mRTTCamera->setViewport(0, 0, 1024, 1024);
```
**Result**: Ignored, viewport is 1280x720 in initial callback

### Fix #2: Force viewport in InitialDrawCallback ⚠️
```cpp
state->applyAttribute(vp);  // Forces to 1024x1024
```
**Result**: Works temporarily, but reverts to 800x600 by final callback

### Fix #3: Add viewport to StateSet with PROTECTED ❌
```cpp
cameraState->setAttributeAndModes(rttViewport,
    osg::StateAttribute::ON |
    osg::StateAttribute::OVERRIDE |
    osg::StateAttribute::PROTECTED);
```
**Result**: No effect, viewport still changes to 800x600

---

## 🔍 Possible Root Causes

### Theory #1: OpenMW Viewport Management
**Likelihood**: 🟢 **HIGH**

OpenMW might have custom viewport handling that:
- Overrides all camera viewports for consistency
- Ignores PROTECTED flags
- Resets viewport after each render pass
- Uses a different mechanism than OSG's standard viewport

**Test**: Search OpenMW codebase for viewport management code

### Theory #2: OSG Render Stages
**Likelihood**: 🟡 **MEDIUM**

OSG might have multiple render stages that each reset the viewport:
- Cull stage: Uses one viewport
- Draw stage: Uses another viewport
- Post-draw stage: Resets to default

**Test**: Add callbacks to every possible render stage

### Theory #3: PRE_RENDER Camera Special Handling
**Likelihood**: 🟡 **MEDIUM**

PRE_RENDER cameras might be treated differently:
- Might use a different viewport mechanism
- Might not respect StateSet attributes
- Might be rendered with main window's context

**Test**: Try POST_RENDER or NESTED_RENDER instead

### Theory #4: OpenMW's Custom Renderer
**Likelihood**: 🟢 **HIGH**

OpenMW might have a custom rendering pipeline that:
- Intercepts all viewport changes
- Forces specific viewports for all cameras
- Doesn't use standard OSG viewport mechanism

**Test**: Look at how OpenMW handles other RTT cameras (water reflections, shadow maps)

### Theory #5: Multiple Render Contexts
**Likelihood**: 🔴 **LOW**

The camera might be rendering in multiple contexts:
- One context with 1024x1024 (correct)
- Another context with 800x600 (wrong)
- Final callback might be in the wrong context

**Test**: Check graphics context in callbacks

---

## 💡 Proposed Solutions

### Solution A: Find and Study OpenMW's RTT Examples
**Effort**: Low | **Chance of Success**: High

OpenMW already has RTT cameras for:
- **Water reflections** - Renders scene to texture for water surface
- **Shadow maps** - Renders depth to texture for shadows
- **Maybe post-processing** - Effects like bloom, SSAO

**Action**:
1. Search for existing RTT camera implementations in OpenMW
2. See how they handle viewport
3. Copy their approach exactly

**Files to check**:
- `apps/openmw/mwrender/` - Rendering code
- Look for "Camera", "RTT", "RenderToTexture", "reflection", "shadow"

### Solution B: Force Viewport at Geometry Level
**Effort**: Medium | **Chance of Success**: Medium

Instead of setting viewport at camera level, set it on the geometry:
- Add a custom Drawable that sets viewport before drawing
- Override the geometry's draw callback
- Force viewport immediately before quad render

**Code**:
```cpp
struct ViewportDrawCallback : public osg::Drawable::DrawCallback
{
    osg::ref_ptr<osg::Viewport> vp;

    virtual void drawImplementation(osg::RenderInfo& renderInfo,
                                    const osg::Drawable* drawable) const
    {
        renderInfo.getState()->applyAttribute(vp.get());
        drawable->drawImplementation(renderInfo);
    }
};

mFootprintQuad->setDrawCallback(new ViewportDrawCallback(rttViewport));
```

### Solution C: Use FinalDrawCallback to Set Viewport
**Effort**: Low | **Chance of Success**: Low

Currently we're only CHECKING viewport in final callback. What if we SET it there too?
- Force viewport in both initial AND final callbacks
- Maybe one of them will work

**Code**:
```cpp
// In FinalDrawCallback:
state->applyAttribute(vp);  // Force viewport again
```

**Problem**: Final callback runs AFTER rendering, so this probably won't help

### Solution D: Bypass OSG Camera Abstraction
**Effort**: High | **Chance of Success**: High

Instead of using osg::Camera for RTT, do it manually:
- Create FBO directly with OpenGL
- Bind FBO before rendering
- Render quad manually
- Set viewport manually with raw OpenGL

**Pros**: Complete control over everything
**Cons**: More complex, platform-specific, harder to maintain

### Solution E: Use osg::Image as Render Target
**Effort**: Medium | **Chance of Success**: Medium

Instead of rendering to texture, render to an osg::Image:
```cpp
osg::ref_ptr<osg::Image> image = new osg::Image;
image->allocateImage(1024, 1024, 1, GL_RGBA, GL_UNSIGNED_BYTE);
mRTTCamera->attach(osg::Camera::COLOR_BUFFER, image.get());
```

OSG might handle viewport differently for image targets vs texture targets.

### Solution F: Ask for Help
**Effort**: Low | **Chance of Success**: ?

Post on:
- OpenMW forums/Discord
- OSG mailing list
- Stack Overflow

Someone might have encountered this exact issue before.

---

## 🎯 Recommended Next Steps

### Step 1: **Search OpenMW's RTT Implementation** (1 hour)
Look for water reflections, shadow maps, or any other RTT usage in OpenMW.
See how they handle viewport.

**Expected Files**:
```
apps/openmw/mwrender/water.cpp
apps/openmw/mwrender/shadows.cpp
apps/openmw/mwrender/renderingmanager.cpp
```

**If found**: Copy their exact approach
**If not found**: Move to Step 2

### Step 2: **Add Viewport to Geometry DrawCallback** (2 hours)
Force viewport at the lowest possible level - right before drawing the quad.

**If this works**: Problem solved!
**If this fails**: Move to Step 3

### Step 3: **Test with Different Render Order** (30 min)
Try POST_RENDER instead of PRE_RENDER:
```cpp
mRTTCamera->setRenderOrder(osg::Camera::POST_RENDER);
```

**If this works**: Problem solved!
**If this fails**: Move to Step 4

### Step 4: **Manual FBO Approach** (1 day)
Bypass osg::Camera entirely and use raw OpenGL FBO.

**This WILL work** but requires more code and maintenance.

---

## 📊 Debug Information Needed

To diagnose further, we need:

### 1. Viewport Trace Throughout Render
Add logging at MORE points:
- After cull callback
- In geometry's cull callback
- In geometry's draw callback
- Before/after each primitive set draw

### 2. OpenMW Viewport Code Audit
Search OpenMW codebase for:
- `glViewport` calls
- `setViewport` calls
- Viewport override mechanisms
- Custom rendering code

### 3. Graphics Context Information
Check if:
- Camera uses correct graphics context
- Context matches texture's context
- Context viewport settings

### 4. OSG State Dump
Log the entire OSG state:
- All StateSet attributes
- All modes
- Inheritance chain
- Override flags

---

## 🚦 Success Criteria

We'll know we've solved this when:

1. ✅ Initial callback viewport = 1024x1024
2. ✅ Final callback viewport = 1024x1024 (stays the same!)
3. ✅ Max depth byte > 0 (quad is rendering)
4. ✅ Saved image shows red pixels
5. ✅ Terrain shows visible deformation

---

## 🎓 What We've Learned

### About OSG:
- RTT cameras can be complex
- Viewport management has multiple layers
- PROTECTED flag doesn't always work
- Callbacks give us visibility into render pipeline

### About Debugging:
- Comprehensive diagnostics reveal issues
- Multiple test approaches help isolate problems
- Documentation is crucial for complex issues
- Systematic testing beats random changes

### About the Problem:
- The issue is NOT:
  - ❌ FBO completeness
  - ❌ Camera traversal
  - ❌ Callback execution
  - ❌ Texture format
  - ❌ Clear operation (tested with clear disabled)

- The issue IS:
  - ✅ **Viewport being overridden during render**
  - ✅ **Need to find WHERE and prevent it**

---

## 📝 Questions to Answer

1. Where is the 800x600 viewport coming from?
2. Why does PROTECTED flag not work?
3. How do other OpenMW RTT cameras handle this?
4. Is there OpenMW-specific viewport management?
5. Should we be using a different render order?
6. Can we set viewport at geometry level instead?
7. Is there a way to disable viewport inheritance?

---

## 💪 Why This Will Be Solved

**Reasons for optimism**:
1. We've made significant progress (identified the exact issue)
2. We have good diagnostics in place
3. The problem is well-understood (viewport override)
4. Multiple potential solutions exist
5. OpenMW must handle RTT somewhere (water, shadows)
6. Community might have encountered this before
7. Worst case: manual FBO approach will definitely work

**This is a solvable problem.** We just need to find the right approach for OpenMW's specific rendering pipeline.

---

**Next step**: Search OpenMW's codebase for existing RTT camera implementations and copy their approach! 🔍
