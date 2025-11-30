# Investigation Report: Water Ripples vs. Snow Deformation

## Objective
Investigate the "Water Ripples" system from commit `bb3b3eb5e498183ae8c804810d6ebdba933dbeb2` and determine what can be reused for the current Snow Deformation system, as per `simplificationcascade.md`.

## Findings

### 1. Water Ripples System (`apps/openmw/mwrender/ripples.cpp`)
The historical system was indeed a fully functional RTT simulation loop.
*   **Architecture:** Implemented as `class Ripples : public osg::Camera`. It encapsulates the entire simulation (FBOs, Textures, Shaders) within a single scene graph node.
*   **Mechanism:** Uses **Ping-Pong Buffering** (swapping read/write textures) to allow the simulation to evolve over time.
*   **Features:**
    *   **Scrolling:** Handles player movement by offsetting the simulation (sliding window).
    *   **Compute Shader Support:** Has a robust fallback mechanism (Compute Shader if available, Fragment Shader otherwise).
    *   **Simulation:** Implements a wave equation in `ripples_simulate.frag`.
    *   **Input:** Uses an explicit `emit()` function to push "blobs" (disturbances) into the simulation via a uniform array.

### 2. Current Snow Deformation (`components/terrain/snowdeformation.cpp`)
The current system re-implements many of the same mechanisms but with a different architecture.
*   **Architecture:** `SnowDeformationManager` (not a Node) creates and manages multiple `osg::Camera`s (`mUpdateCamera`, `mDepthCamera`, `mBlurH/VCamera`) and adds them to the scene graph.
*   **Mechanism:** Also uses **Ping-Pong Buffering** (`mAccumulationMap[2]`) and explicit buffer swapping.
*   **Features:**
    *   **Scrolling:** Implements the same sliding window logic.
    *   **Depth Camera:** Adds a crucial feature missing from Ripples: a camera that renders *actors* into a mask, allowing for continuous deformation by presence rather than just discrete events.
    *   **Blur:** Adds Gaussian blur passes.

## Comparison & Reuse Opportunities

The developer was correct: the core "Ping-Pong RTT with Scrolling" logic was re-implemented. However, the Snow system requires the **Depth Camera** input, which the Ripples system did not have (it used a uniform array of points).

### What we can reuse (Simplification Opportunities):

1.  **Architectural Pattern (High Priority):**
    *   The `Ripples` class is cleaner. It inherits from `osg::Camera` and manages its own internal state (FBOs, Quads).
    *   **Proposal:** Refactor `SnowDeformationManager` to contain a `class SnowSimulation : public osg::Camera`. This would encapsulate the Ping-Pong logic, FBO creation, and Update Shader, cleaning up the Manager significantly.

2.  **Compute Shader Support (Medium Priority):**
    *   The `Ripples` code has a ready-made structure for supporting Compute Shaders.
    *   **Proposal:** Copy the `setupComputePipeline` and `setupFragmentPipeline` logic. Compute shaders are generally faster for this kind of simulation (texture writes) and avoid some FBO overhead.

3.  **Simulation Logic (Low Priority):**
    *   The `ripples_simulate.frag` contains a wave equation.
    *   **Proposal:** If we want "dynamic" snow (e.g., filling back in over time like a fluid, or avalanches), we can adapt this shader. For now, the Snow system uses a simple linear decay, which is appropriate for static snow.

## Conclusion
The "Water Ripples" system is a better *architectural* implementation (cleaner encapsulation), but the "Snow Deformation" system has the necessary *features* (Depth Camera input).

**Recommendation:**
Do not revert to the old code directly. Instead, **refactor the Snow system to follow the `Ripples` architectural pattern**. Encapsulate the RTT/Ping-Pong logic into a dedicated `osg::Camera` subclass (e.g., `SnowSimulationCamera`). This will:
1.  Reduce the complexity of `SnowDeformationManager`.
2.  Make it easier to add Compute Shader support (by copying the pattern from `Ripples`).
3.  Fix the "buggy" nature by isolating the simulation logic from the scene management.


# Implementation Plan - Snow Architecture Refactor

## Goal
Refactor the `SnowDeformationManager` to separate the **Simulation Logic** (RTT, Ping-Pong, Shaders) from the **Scene Management** (Setup, Detection). We will adopt the architecture of the `Ripples` system, creating a dedicated `SnowSimulation` class.

## User Question: Array vs. Depth
> *Can you explain the "array vs depth" part?*

**Ripples (Array):** The historical system used a list of points (`std::array<Vec3>`).
- **How it worked:** The CPU sent a list of "splash positions" to the shader (e.g., `uniform vec3 drops[100]`).
- **Shader:** For every pixel, it calculated the distance to all 100 points.
- **Result:** Perfect for circular raindrops. Bad for complex shapes (feet, bodies) or continuous trails, as you'd need thousands of points.

**Snow (Depth):** The current system uses a **Depth Camera**.
- **How it works:** We render the actual 3D geometry of actors (feet, wheels) from below into a texture (`ObjectMask`).
- **Shader:** It just reads this texture. If a pixel is white, it pushes the snow down.
- **Result:** Captures the *exact shape* of the object and supports continuous movement (dragging feet) for free.

**Refactoring Implication:**
We will keep the **Depth Camera** in `SnowDeformationManager` (as the "Eye" that sees objects) and pass its result (the `ObjectMask` texture) to the new `SnowSimulation` class (the "Brain" that remembers the trails).

## Proposed Changes

### 1. New Class: `SnowSimulation` (`components/terrain/snowsimulation.hpp/cpp`)
This class will encapsulate the "Ping-Pong" simulation loop.
- **Inheritance:** `osg::Camera` (like `Ripples`) or `osg::Group` containing the cameras.
    - *Decision:* Inheriting `osg::Camera` is tricky because we have multiple passes (Update, Blur H, Blur V). `Ripples` did it because it was a single pass (mostly).
    - *Refined Approach:* `SnowSimulation` will be a `osg::Group` that contains the RTT Cameras (`mUpdateCamera`, `mBlurH`, `mBlurV`) as children.
- **Responsibilities:**
    - Manage the 2 Accumulation Textures (Ping-Pong).
    - Manage the Blur Textures.
    - Setup the Shaders (`snow_update`, `blur`).
    - Handle the "Scrolling" logic (moving the simulation window).
- **Inputs:**
    - `ObjectMask` texture (from `SnowDeformationManager`).
    - `PlayerPosition` (for scrolling).

### 2. Refactor: `SnowDeformationManager`
- **Remove:**
    - `mAccumulationMap`, `mBlurTempBuffer`, `mBlurredDeformationMap`.
    - `mUpdateCamera`, `mBlurHCamera`, `mBlurVCamera`.
    - `mUpdateQuad`, `mBlurHQuad`, `mBlurVQuad`.
    - `updateRTT()` logic.
- **Keep:**
    - `mDepthCamera` (The sensor).
    - `mObjectMaskMap` (The sensor output).
    - `SnowDetection` logic (When to enable).
- **Add:**
    - `osg::ref_ptr<SnowSimulation> mSimulation;`
    - In `update()`: Update `mDepthCamera` position, then call `mSimulation->update(pos, dt)`.

### 3. Build System
- Add `snowsimulation.cpp` and `snowsimulation.hpp` to `components/terrain/CMakeLists.txt`.

## Verification Plan
- **Compile:** Ensure no linker errors.
- **Visual Test:**
    1.  Run the game.
    2.  Walk in snow.
    3.  Verify trails still appear and persist.
    4.  Verify debug overlay (if enabled) shows the textures.
