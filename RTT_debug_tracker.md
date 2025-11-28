# Snow Deformation RTT Debug Tracker

**Objective:** Fix the regression in the snow deformation system where the entire RTT area is being depressed instead of just footprints.

**Current Status:** DEBUGGING - Full-area depression bug identified, CullCallback approach replaced

---

## Current Issue: Full-Area RTT Depression

### Symptom
The entire RTT camera area is being depressed as a square, rather than just where the player/NPCs are standing.

### Visual Behavior

**In Wireframe Mode:**
- Vertices appear **NOT** deformed
- They look at the same altitude as before deformation
- Proves the **vertex shader is NOT displacing vertices down**

**In Regular Mode:**
- Deformation appears as a **square-shaped depression**
- The square matches the RTT camera zone
- Proves the **fragment shader POM is creating the visual effect**

### Root Cause Analysis

**Vertex Shader Logic (terrain.vert:98-99):**
```glsl
// Apply deformation: raise terrain by baseLift, then subtract where footprints are
vertex.z += baseLift * (1.0 - vDeformationFactor);
```

**This means:**
- When `vDeformationFactor = 0` (no deformation): `vertex.z += baseLift` → **RAISED**
- When `vDeformationFactor = 1` (full deformation): `vertex.z += 0` → **unchanged**

**The Problem:** The object mask texture (`mObjectMaskMap`) was **FULL WHITE** across the entire RTT area, making `vDeformationFactor = 1.0` everywhere. This causes the terrain to not be raised (staying at original height) inside the RTT zone, while terrain outside is raised by `baseLift`, creating a visual "depression" square.

---

## Investigation History

### Phase 1: Initial Investigation (Session 1)
1. **Texture Binding Fix** ✅ COMPLETE
   - Issue: `snowDeformationMap` uniform was being set to texture pointer (invalid for samplers)
   - Fix: Exposed `getDeformationMap()` and bound texture to Unit 7
   - Result: RTT debug visualization became visible

2. **Unit Scale Correction** ✅ COMPLETE
   - `mRTTSize` increased from `50.0` to `3625.0` (approx 50 meters in Morrowind units)
   - `mFootprintInterval` increased from `2.0` to `45.0` (approx 2 feet)

3. **Camera Setup Fixes** ✅ COMPLETE
   - Removed `setViewMatrix(identity())` that was overwriting LookAt
   - Fixed projection matrix Z-range
   - Fixed initialization of `mRootNode`

### Phase 2: Depth Camera Investigation (Session 2)
4. **Depth Camera Traversal Issue** 🔴 IN PROGRESS

   **Discovery:** The `DepthCameraCullCallback` was traversing ALL scene nodes regardless of cull mask compatibility.

   **Log Evidence:**
   ```
   [23:47:52.597] Depth camera rendering child: Sky Root (mask: 4294967295)
   [23:47:52.597] Depth camera rendering child: Water Root (mask: 4294967295)
   [23:47:52.597] Depth camera rendering child: Cell Root (mask: 4294967295)
   [23:47:52.597] Depth camera rendering child: Player Root (mask: 16)
   ```

   **Analysis:**
   - Node mask `4294967295` = `0xFFFFFFFF` (all bits set)
   - Depth camera cull mask: `(1 << 3) | (1 << 4) | (1 << 10)` = bits 3, 4, 10
   - When ANDing `0xFFFFFFFF & 1048`, result is `1048` (non-zero) → nodes pass filter
   - BUT: Their children (Sky, Water, etc.) should be culled by OSG's normal culling
   - Problem: `child->accept(*nv)` bypasses normal cull mask filtering

5. **Attempted Fix: Manual Cull Mask Filtering** ❌ FAILED
   - Added manual node mask checking before `accept()`
   - Result: Still renders all nodes because root nodes have all bits set
   - Issue: Need OSG to filter children, not just parent nodes

6. **Current Approach: Direct Scene Graph Attachment** 🔄 TESTING
   - Disabled `DepthCameraCullCallback` entirely
   - Added Player Root and Cell Root as direct children of depth camera
   - Code location: `snowdeformation.cpp:612-630`
   - Expected: Camera's cull mask will now properly filter children
   - Status: **Built successfully, awaiting user testing**

---

## Technical Details

### Depth Camera Configuration
**Location:** `snowdeformation.cpp:515-540`

```cpp
// Clear Color: Black (0,0,0,0) = No objects
mDepthCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));

// Cull Mask: Actor(3) | Player(4) | Object(10)
mDepthCamera->setCullMask((1 << 3) | (1 << 4) | (1 << 10));

// Shader Override: Output white for all rendered pixels
dProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT,
    "void main() {\n"
    "  gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
    "}\n"));
```

### Current Workaround (snowdeformation.cpp:612-630)
```cpp
// Add Player Root and Cell Root as direct children
if (mRootNode && mDepthCamera)
{
    for (unsigned int i = 0; i < mRootNode->getNumChildren(); ++i)
    {
        osg::Node* child = mRootNode->getChild(i);
        std::string name = child->getName();

        if (name == "Player Root" || name == "Cell Root")
        {
            Log(Debug::Info) << "Adding " << name << " to depth camera scene";
            mDepthCamera->addChild(child);
        }
    }
}
```

---

## Node Mask Reference (from vismask.hpp)

```cpp
Mask_Actor = (1 << 3)        = 8       = 0x00000008
Mask_Player = (1 << 4)       = 16      = 0x00000010
Mask_Sky = (1 << 5)          = 32      = 0x00000020
Mask_Terrain = (1 << 8)      = 256     = 0x00000100
Mask_Object = (1 << 10)      = 1024    = 0x00000400

Depth Camera Mask = 8 | 16 | 1024 = 1048 = 0x00000418
```

---

## SOLUTION IMPLEMENTED (2025-11-29) ✅

### Fix Applied: Attached DepthCameraCullCallback

**The Problem:**
- `DepthCameraCullCallback` class was defined but never attached to the camera
- Workaround at lines 612-630 tried to add Player/Cell roots as children → circular reference
- Depth camera couldn't see any actors → empty object mask → no deformation

**The Solution:**
1. **Removed circular reference workaround** (deleted lines 612-630)
2. **Attached CullCallback properly:** `mDepthCamera->setCullCallback(new DepthCameraCullCallback(mRootNode, mDepthCamera))`
3. **Simplified callback logic:** Removed broken manual mask filtering, let OSG's CullVisitor handle it automatically

**How It Works:**
- Depth camera is sibling to Player/Cell/Terrain roots in scene graph
- CullCallback manually traverses `mRootNode`'s children during depth camera's cull pass
- Skips: Camera itself (avoid recursion), Terrain Root (avoid self-deformation)
- For each child, calls `child->accept(*nv)` → OSG's CullVisitor filters descendants by node mask
- Only Actor (bit 3), Player (bit 4), Object (bit 10) nodes get rendered
- Result: Small white silhouette of player/NPCs in object mask texture

### Immediate Testing Needed
1. ⬜ Build with new changes (2025-11-29)
2. ⬜ Test in-game to verify:
   - Full-area depression is GONE
   - Only player footprints appear where you walk
   - NPCs/creatures also leave trails
3. ⬜ Check logs for:
   - "SnowDeformationManager: Attached DepthCameraCullCallback to depth camera"
   - Should NOT see circular reference errors or crash

### If Current Fix Works
1. ✅ CullCallback is now properly used (not removed!)
2. ⬜ Test with multiple NPCs to verify they all leave trails
3. ⬜ Test on slopes to verify depth range (200 units) is sufficient

### If Current Fix Fails
**Alternative approaches to try:**

1. **Use RTT Camera's Inherit Viewpoint Mode**
   - Change `setReferenceFrame(ABSOLUTE_RF)` to `ABSOLUTE_RF_INHERIT_VIEWPOINT`
   - May allow depth camera to see scene without manual attachment

2. **Use a Separate Scene Graph**
   - Clone Player/Actor geometry each frame
   - Render clones to depth camera
   - More overhead but guaranteed isolation

3. **Use Depth Buffer Directly**
   - Instead of rendering to object mask, read main camera's depth buffer
   - Compare depth to terrain height
   - More complex but may be more accurate

---

## Known Files Modified

### C++ Files
- `components/terrain/snowdeformation.cpp` - Main implementation
  - Added `#include <osgUtil/CullVisitor>`
  - Modified `DepthCameraCullCallback` (currently disabled)
  - Added direct scene graph attachment (lines 612-630)
  - Changed depth range to 200.0 (was 2000.0)

### Shader Files
- `files/shaders/compatibility/terrain.vert` - No changes (reverted)
- `files/shaders/compatibility/terrain.frag` - No changes

---

## Debug Artifacts

### Mystery File
**File:** `"d\357\200\272GamedevOpenMWopenmw-dev-mastersnowdeformation_old.cpp"`
- Strange filename with Unicode characters
- Appears to be a backup/old version
- Location: Project root (should be in `components/terrain/`)
- **Action needed:** Investigate and delete if unnecessary

**Analysis:**
- `\357\200\272` is UTF-8 encoding for `:` (U+F03A - Private Use Area)
- Suggests a filename encoding issue
- Likely created accidentally, possibly by editor or file system error
- **Recommendation:** Delete this file - it's garbage data

---

## References
- `rtt_dom_particles.md:98-134` - Depth camera architecture
- `RTT_FULL_AREA_BUG.md` - Detailed diagnostic report (created this session)
- `snowdeformation.cpp:515-585` - Depth camera setup
- `terrain.vert:98-99` - Vertex displacement logic
- `vismask.hpp:22-59` - Node mask definitions

---

## Status Summary

**Issue:** Entire RTT area depressed instead of just footprints
**Root Cause:** Depth camera not rendering actors (CullCallback never attached)
**Solution:** Attach DepthCameraCullCallback + remove circular reference workaround
**Build Status:** ⬜ Awaiting rebuild (2025-11-29)
**Test Status:** ⬜ Awaiting user testing
**Priority:** HIGH - Should fix core snow deformation functionality

---

**Last Updated:** 2025-11-29
**Session:** 3 (Comprehensive Audit + Fix)
**Next Step:** Build and test in-game to verify footprints now work correctly
