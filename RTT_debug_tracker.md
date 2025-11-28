# Snow Deformation RTT Debug Tracker

**Objective:** Fix the regression in the snow deformation system where footprints are not appearing.
**Current Status:** RTT system is **WORKING**. Red square and footprints are visible for the player. Terrain deformation is occurring.
**New Issue:** NPCs and creatures do not leave trails.

## Recent Fixes & Changes
1.  **Texture Binding Fix**:
    *   **Issue**: `snowDeformationMap` uniform was being set to the texture pointer (which is invalid for samplers) and the texture was never bound to a texture unit on the terrain StateSet.
    *   **Fix**:
        *   Exposed `getDeformationMap()` in `SnowDeformationManager`.
        *   Updated `SnowDeformationManager` to set the uniform to `int(7)` (Texture Unit 7).
        *   Updated `SnowDeformationUpdater` to bind the texture to Unit 7 using `setTextureAttributeAndModes`.
    *   **Result**: RTT debug visualization is now visible.

## Current Symptoms
1.  **Unit Scale Correction**:
    *   `mRTTSize` increased from `50.0` to `3625.0` (approx 50 meters in Morrowind units, where 22.1 units = 1 foot).
    *   `mFootprintInterval` increased from `2.0` to `45.0` (approx 2 feet).
    *   *Reasoning*: Previous values were too small (sub-meter), likely making the RTT cover a tiny area.
2.  **RTT Camera Setup**:
    *   **View Matrix**: Removed `mRTTCamera->setViewMatrix(osg::Matrix::identity())` which was overwriting the `LookAt` matrix.
    *   **Projection**: Changed to `setProjectionMatrixAsOrtho` with Z-range `0.0` to `20000.0` (camera is at Z=10000).
    *   *Reasoning*: Ensure camera looks down from high up and captures geometry at Z=0 without clipping.
3.  **Initialization**:
    *   Added `mRootNode(rootNode)` to `SnowDeformationManager` constructor initialization list (User fix).
    *   *Reasoning*: `mRootNode` might have been uninitialized, preventing the camera from being attached to the scene graph.
4.  **Texture Binding Fix**:
    *   **Issue**: `snowDeformationMap` uniform was being set to the texture pointer (which is invalid for samplers) and the texture was never bound to a texture unit on the terrain StateSet.
    *   **Fix**:
        *   Exposed `getDeformationMap()` in `SnowDeformationManager`.
        *   Updated `SnowDeformationManager` to set the uniform to `int(7)` (Texture Unit 7).
        *   Updated `SnowDeformationUpdater` to bind the texture to Unit 7 using `setTextureAttributeAndModes`.
    *   **Expected Result**: Should now see Dark Red square (Clear Color) and Bright Red footprints.

## Current Symptoms
*   **No Green/Red Square**: The RTT clear color (set to Dark Red `0.2, 0, 0, 1`) is not visible on the terrain.
*   **No Footprints**: Red footprints are not visible.
*   **Conclusion**: The RTT texture (`mDeformationMap`) is either:
    *   Not being rendered to (Camera not updating/culling).
    *   Not being bound to the terrain shader correctly.
    *   Not being sampled by the terrain shader (UVs off or debug code inactive).

## Next Steps / Debug Plan

### 1. Verify Shader Sampling & UVs
*   **Check**: Is `terrain.frag` actually using the debug code?
*   **Action**: Hardcode a color in `terrain.frag` (e.g., `gl_FragColor = vec4(0,1,0,1);`) to confirm we are editing the correct shader and it's recompiling.
*   **Action**: Check `deformUV` calculation in `terrain.vert`. If `snowRTTScale` or `snowRTTWorldOrigin` are wrong, we might be sampling outside the texture (clamped to border -> transparent).

### 2. Verify Texture Binding
*   **Check**: Is `snowDeformationMap` uniform actually pointing to the texture unit where `mDeformationMap` is bound?
*   **Action**: In `SnowDeformationUpdater::setDefaults`, ensure the uniform is created and added.
*   **Action**: Use a tool like RenderDoc (if available) or add OSG debug output to check StateSet application.

### 3. Verify RTT Camera Update
*   **Check**: Is the RTT camera actually traversing?
*   **Action**: Add a `DrawCallback` to the RTT camera to log when it draws.
*   **Action**: Ensure `mRootNode` is actually part of the active scene graph.

### 4. Verify Uniform Propagation
*   **Check**: Are `snowRTTScale` and `snowRTTWorldOrigin` reaching the shader?
*   **Action**: Visualize these values in the shader (e.g., output color based on `snowRTTScale`).

## Key Files
*   `components/terrain/snowdeformation.cpp` (Manager logic, RTT setup)
*   `components/terrain/snowdeformationupdater.cpp` (Uniform setup)
*   `files/shaders/compatibility/terrain.frag` (Fragment shader, debug viz)
*   `files/shaders/compatibility/terrain.vert` (Vertex shader, UV calc)
