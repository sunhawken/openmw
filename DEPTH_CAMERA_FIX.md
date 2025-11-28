# Depth Camera Fix - 2025-11-29

## Problem Summary

The snow deformation system was showing a "full-area depression" bug where the entire RTT area (50m × 50m) appeared depressed instead of showing individual footprints.

## Root Cause

The **depth camera was not rendering any actors**, resulting in an empty object mask texture. This happened because:

1. The `DepthCameraCullCallback` class was **defined but never attached** to the camera
2. A workaround at [snowdeformation.cpp:612-630](d:\Gamedev\OpenMW\openmw-dev-master\components\terrain\snowdeformation.cpp#L612-L630) tried to add "Player Root" and "Cell Root" as direct children of the depth camera
3. This created a **circular scene graph reference** (nodes can't have multiple parents in OSG)
4. The camera had no valid scene to render → empty object mask → no deformation data

## The Fix

### Change 1: Attach the CullCallback

**File:** [snowdeformation.cpp:598-616](d:\Gamedev\OpenMW\openmw-dev-master\components\terrain\snowdeformation.cpp#L598-L616)

**Before:**
```cpp
// Add cameras to scene graph
if (mRootNode)
{
    mRootNode->addChild(mDepthCamera);
    mRootNode->addChild(mUpdateCamera);
    mRootNode->addChild(mRTTCamera);
    mRootNode->addChild(mBlurHCamera);
    mRootNode->addChild(mBlurVCamera);
}

// WORKAROUND: Add Player Root and Cell Root as children (BROKEN!)
if (mRootNode && mDepthCamera)
{
    for (unsigned int i = 0; i < mRootNode->getNumChildren(); ++i)
    {
        osg::Node* child = mRootNode->getChild(i);
        if (child->getName() == "Player Root" || child->getName() == "Cell Root")
        {
            mDepthCamera->addChild(child);  // ❌ Circular reference!
        }
    }
}
```

**After:**
```cpp
// Add cameras to scene graph
if (mRootNode)
{
    mRootNode->addChild(mDepthCamera);
    mRootNode->addChild(mUpdateCamera);
    mRootNode->addChild(mRTTCamera);
    mRootNode->addChild(mBlurHCamera);
    mRootNode->addChild(mBlurVCamera);

    // SOLUTION: Attach CullCallback to allow depth camera to see scene
    mDepthCamera->setCullCallback(new DepthCameraCullCallback(mRootNode, mDepthCamera));
    Log(Debug::Info) << "SnowDeformationManager: Attached DepthCameraCullCallback to depth camera";
}
```

### Change 2: Simplify CullCallback Logic

**File:** [snowdeformation.cpp:36-65](d:\Gamedev\OpenMW\openmw-dev-master\components\terrain\snowdeformation.cpp#L36-L65)

**Before:**
```cpp
virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
{
    traverse(node, nv);

    if (mRoot)
    {
        // Buggy code trying to get cull mask
        unsigned int cameraCullMask = cv->getCullingMode(); // ❌ Wrong!

        // Manual node mask filtering
        for (unsigned int i = 0; i < mRoot->getNumChildren(); ++i)
        {
            osg::Node* child = mRoot->getChild(i);

            if (child == mCam) continue;
            if (child->getName() == "Terrain Root") continue;

            // Check mask manually (doesn't work for nested nodes!)
            if ((nodeMask & depthCameraMask) == 0) continue;

            child->accept(*nv);
        }
    }
}
```

**After:**
```cpp
virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
{
    traverse(node, nv);

    // Manually traverse the Scene Root's children (Siblings of this camera)
    // The CullVisitor will automatically filter children based on the camera's cull mask
    if (mRoot && nv)
    {
        for (unsigned int i = 0; i < mRoot->getNumChildren(); ++i)
        {
            osg::Node* child = mRoot->getChild(i);

            // Skip the Camera itself to avoid infinite recursion
            if (child == mCam)
                continue;

            // Skip the Terrain to prevent self-deformation
            if (child->getName() == "Terrain Root")
                continue;

            // Let OSG's normal culling handle node mask filtering
            // CullVisitor will check (child->getNodeMask() & camera->getCullMask())
            // This properly traverses into the child hierarchy and filters descendants
            child->accept(*nv);
        }
    }
}
```

## How It Works Now

1. **Depth camera is a sibling** in the scene graph (child of `mRootNode`)
2. **CullCallback executes** during the camera's cull pass
3. **Callback manually traverses** `mRootNode`'s children:
   - Skips: Camera itself (avoid recursion), Terrain Root (avoid self-deformation)
   - For others: Calls `child->accept(*nv)`
4. **OSG's CullVisitor filters descendants** based on camera's cull mask `(1<<3)|(1<<4)|(1<<10)`
5. **Only matching nodes render:**
   - Mask_Actor (bit 3) = Creatures, NPCs
   - Mask_Player (bit 4) = Player character
   - Mask_Object (bit 10) = Dynamic objects
6. **Shader outputs white** for all rendered pixels
7. **Result:** Object mask texture contains white silhouettes of actors

## Expected Behavior After Fix

### What Should Happen

1. **Object Mask Texture (`mObjectMaskMap`):**
   - Black background (no objects)
   - Small white silhouette where player stands (~2m circle)
   - White silhouettes for NPCs/creatures near player

2. **Accumulation Buffer (`mAccumulationMap`):**
   - White trails accumulating where player walks
   - Trails fade over time (decay)
   - Trails shift with sliding window (follow player)

3. **Blurred Deformation Map (`mBlurredDeformationMap`):**
   - Smooth gaussian-blurred footprint trails
   - Read by terrain shaders for vertex displacement

4. **In-Game Visual:**
   - Footprints appear where you walk
   - Terrain vertices displaced downward (vertex shader)
   - POM adds visual depth (fragment shader)
   - Normal maps updated (lighting reacts to depressions)
   - Darkened snow in footprints (compressed/wet look)

### What to Check

**Log Messages:**
```
[Info] SnowDeformationManager: Attached DepthCameraCullCallback to depth camera
```

**In-Game:**
- Walk in snow → see footprints behind you ✅
- Footprints fade after ~3 minutes (default decay time)
- NPCs/creatures leave trails too
- No more full-area depression ✅

**Debugging (if still broken):**
- Enable debug visualization in terrain.frag (lines 65-71)
- Green tint = inside RTT area
- Red overlay = deformation present (should only be on footprints)

## Technical Details

### Why Manual Mask Filtering Didn't Work

The old code tried to filter at the root level:
```cpp
unsigned int nodeMask = child->getNodeMask();
if ((nodeMask & depthCameraMask) == 0) continue;
```

**Problem:** Root nodes like "Player Root" and "Cell Root" typically have `nodeMask = 0xFFFFFFFF` (all bits set). They always pass the filter!

**What we needed:** Filter at the **descendant** level (actual actor geometry nodes have specific masks).

**Solution:** Let `child->accept(*nv)` invoke OSG's normal culling, which traverses into the hierarchy and filters descendants properly.

### Why the Circular Reference Failed

OSG's scene graph is a **DAG (Directed Acyclic Graph)**, not a true graph:
- Nodes can have multiple parents IF using `osg::Group` reference counting carefully
- BUT RTT cameras with specific cull masks don't work well with shared parents
- The camera's cull traversal expects a clean hierarchy

**The workaround tried:**
```cpp
mDepthCamera->addChild(playerRoot);  // playerRoot is ALSO child of mRootNode!
```

**This causes:**
- Confusing traversal order
- Cull mask may not apply correctly
- Potential crashes or undefined behavior

**The proper solution (CullCallback):**
- Camera remains a clean sibling
- Callback manually visits siblings during cull pass
- No circular reference, no shared parents

## Files Modified

**Modified:**
- [components/terrain/snowdeformation.cpp](d:\Gamedev\OpenMW\openmw-dev-master\components\terrain\snowdeformation.cpp)
  - Lines 36-65: Simplified `DepthCameraCullCallback::operator()`
  - Lines 598-616: Removed workaround, attached callback

**Unchanged:**
- [components/terrain/snowdeformation.hpp](d:\Gamedev\OpenMW\openmw-dev-master\components\terrain\snowdeformation.hpp) - No changes needed
- Shader files - No changes needed

## Testing Plan

### 1. Build the Project
```bash
cmake --build . --config Debug
```

### 2. Run In-Game
- Load a save in snow terrain (Solstheim)
- Walk around for 30 seconds
- Look behind you

**Expected:** Footprints visible in the snow ✅

### 3. Check Logs
Search for:
```
SnowDeformationManager: Attached DepthCameraCullCallback to depth camera
```

### 4. Test Edge Cases
- Walk on slopes (verify depth range sufficient)
- Walk near NPCs (verify they leave trails too)
- Stand still for 3 minutes (verify trails decay)
- Fast travel (verify sliding window resets properly)

## Potential Issues & Fallbacks

### If Still No Footprints

**Check 1: Object Mask Is Still Empty**

Possible causes:
- Node names don't match (`"Player Root"` vs `"PlayerRoot"`)
- Actor nodes have unexpected masks
- CullVisitor not being invoked

**Debug:**
Add logging to callback:
```cpp
Log(Debug::Info) << "CullCallback traversing: " << child->getName()
                 << " mask=" << child->getNodeMask();
```

**Check 2: Blur Shaders Not Loading**

Symptom: Footprints pixelated or garbage
Solution: Already verified shaders exist (blur_horizontal.frag, blur_vertical.frag)

**Check 3: Depth Range Too Small**

Symptom: Tall NPCs/creatures don't leave trails
Solution: Increase depth range from 200 to 500 in [snowdeformation.cpp:742](d:\Gamedev\OpenMW\openmw-dev-master\components\terrain\snowdeformation.cpp#L742)

### Alternative Approach (If Callback Fails)

If the CullCallback approach still doesn't work, we can use a **pre-draw callback** to clone actor geometry:

```cpp
class DepthCameraPreDrawCallback : public osg::Camera::DrawCallback
{
    virtual void operator()(osg::RenderInfo& renderInfo) const
    {
        // Find actors in scene
        // Clone their geometry
        // Add clones as children of depth camera
    }
};
```

This is more expensive (cloning geometry each frame) but guaranteed to work.

## Conclusion

The fix is simple: **just attach the callback that was already written!**

The `DepthCameraCullCallback` class was carefully designed to solve exactly this problem, but it was never actually used. By attaching it and removing the buggy workaround, the depth camera should now properly render actors, generating the object mask that drives the entire deformation system.

**Estimated Result:** Snow deformation should work correctly after this fix. The full-area depression bug should be completely resolved.

---

**Fix Applied:** 2025-11-29
**Build Required:** Yes
**Breaking Changes:** None (only fixes broken functionality)
**Performance Impact:** None (callback already existed, just not attached)
