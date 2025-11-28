# Snow Deformation System Audit & Fix

## 1. Audit Findings

### The "Whole Area Pushed Down" Bug
**Symptoms:** The user reported that the entire RTT area was being "pushed down" (flattened), rather than just where objects were.
**Root Cause:** The `mDepthCamera` (Object Mask) was likely rendering the **Terrain** itself into the mask.
- The `mDepthCamera` looks up from below the player.
- If the Terrain is rendered into the mask, the mask becomes White (Full Deformation) everywhere.
- The Terrain Shader reads this White mask and applies full deformation (0 lift) to the entire area, making it appear flat/pushed down relative to the raised snow.

### The "NPCs Don't Leave Trails" Bug
**Symptoms:** NPCs and creatures were not generating footprints.
**Root Cause:** The `mDepthCamera` was empty.
- The camera was added to the scene graph (`mRootNode->addChild(mDepthCamera)`), but it had no children of its own.
- In OpenMW/OSG, a camera only renders its subgraph.
- Since it had no children, it rendered nothing (except potentially the Terrain if it was somehow picking it up via parent traversal, though the "Empty" theory contradicts the "White Mask" theory unless the Empty state defaults to White or the Terrain was being picked up by a different mechanism).
- **Correction:** The most likely scenario is that the camera was indeed empty (Black Mask -> Raised Terrain), but the user's description of "Pushed Down" might have been misinterpreted or there was a specific interaction causing the mask to be White. However, the **Fix** addresses both possibilities.

### Missing Shaders
**Issue:** `blur_horizontal.frag` and `blur_vertical.frag` were missing from the codebase, which is a blocker for the blur feature.

## 2. Implemented Solution

### A. Depth Camera Cull Callback
I implemented a `DepthCameraCullCallback` in `snowdeformation.cpp`. This callback solves both bugs:
1.  **Renders Actors:** It manually traverses the `mRootNode`'s children (siblings of the camera) and calls `accept(*nv)` on them. This forces the camera to render the scene actors even though they are not its children.
2.  **Excludes Terrain:** It explicitly checks for the node named `"Terrain Root"` and skips it. This ensures the Terrain is **never** rendered into the Object Mask, preventing the "Whole Area Pushed Down" bug.

```cpp
// In DepthCameraCullCallback::operator()
if (child->getName() == "Terrain Root")
    continue; // Skip Terrain
child->accept(*nv); // Render Actors
```

### B. Created Missing Shaders
I created the missing shader files in `files/shaders/compatibility/`:
- `blur_horizontal.frag`
- `blur_vertical.frag`

## 3. Verification
The system should now work as follows:
1.  **Object Mask:** Will only contain dynamic objects (NPCs, Creatures, Player) because Terrain is skipped.
2.  **Deformation:** Will only occur where objects are present.
3.  **Visuals:** The terrain will be "Raised" (Snowy) by default, and "Pushed Down" (Deformed) only where footprints are.
4.  **Blur:** The footprints will have soft edges due to the new blur shaders.

## 4. Next Steps
- **Build and Run:** Compile the changes and run the game.
- **Test:** Walk around in a snowy area. Verify that:
    - The terrain is not flattened everywhere.
    - NPCs leave trails.
    - Footprints are smooth (blurred).
