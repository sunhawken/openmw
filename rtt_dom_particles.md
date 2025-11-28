# Hybrid Snow Deformation & Particles Tracker

## Overview
We are implementing a **Hybrid Snow Deformation System** inspired by God of War's snow rendering. This system combines three key technologies to achieve realistic, deep snow effects without excessive geometric complexity:

1.  **RTT (Render-To-Texture)**: Generates a dynamic heightmap of footprints around the player.
2.  **Vertex Deformation**: Uses the RTT map to physically lower terrain vertices (creating the silhouette).
3.  **Visuals (POM/Darkening)**: Uses the RTT map in the pixel shader to create high-frequency details (darkening, Parallax Occlusion Mapping) that simulate deep holes even on flat geometry.
4.  **Particles**: Spawns snow/ash/mud spray on footsteps for added impact.

## Analysis & Research
**Reference**: [God of War Snow Rendering](https://mamoniem.com/behind-the-pretty-frames-god-of-war/#6-3snow-mesh)

**Key Findings**:
*   **RTT is King**: GoW uses a dynamic RTT texture to store trail data. This is more scalable than passing arrays to shaders.
*   **Visuals > Geometry**: While they use tessellation, the "deep" look comes primarily from **Parallax Occlusion Mapping (POM)** in the shader, driven by the RTT heightmap.
*   **Hybrid Approach**: We adopted a similar hybrid strategy:
    *   *Vertex Shader*: Lowers the mesh so feet don't clip through.
    *   *Fragment Shader*: Darkens and (planned) parallax-shifts the texture to look like a deep hole.

## Implementation Status

### 1. Particle System (DONE)
*   **Class**: `SnowParticleEmitter` (`components/terrain/snowparticleemitter.hpp/cpp`)
*   **Tech**: Uses `osgParticle` with `RadialShooter` for spray effects.
*   **Integration**: Called from `SnowDeformationManager::stampFootprint`.
*   **Features**: Supports different configs for Snow (white), Ash (grey), and Mud (brown).

### 2. RTT System (DONE - UPGRADED)
*   **Class**: `SnowDeformationManager`
*   **Tech**:
    *   **Persistent Accumulation**: Uses a "Ping-Pong" buffer system (`mAccumulationMap[2]`) to store deformation data indefinitely.
    *   **Update Pass**: A dedicated `mUpdateCamera` runs a shader (`update.frag`) every frame to handle scrolling (sliding window) and time-based decay.
    *   **Depth Projection**: A `mDepthCamera` renders all actors (NPCs, Creatures, Player) from *below* into a mask map. This mask is fed into the update shader to automatically deform snow under any moving character.
*   **Uniforms**: Passes `snowDeformationMap` (Read Buffer), `snowRTTWorldOrigin`, and `snowRTTScale` to terrain shaders.

### 3. Shaders (DONE)
*   **Vertex Shader** (`terrain.vert`):
    *   Samples `snowDeformationMap` to lower vertices (Vertex Displacement).
*   **Fragment Shader** (`terrain.frag`):
    *   **Normal Reconstruction**: Calculates the gradient of the deformation map (using finite differences) to perturb the surface normal. This creates realistic lighting edges and self-shadowing for footprints.
    *   **POM**: Uses the deformation map for Parallax Occlusion Mapping (Raymarching) to simulate deep holes.
    *   **Darkening**: Darkens the diffuse color in deformed areas.

## Remaining Tasks / Tracker

- [x] **Verification**: Test in-game.
    - [x] Verify footprints appear (RTT working).
    - [x] Verify particles spawn.
    - [x] Verify terrain deforms physically.
- [ ] **Parallax Occlusion Mapping (POM)**:
    - [x] Upgrade `terrain.frag` to use the RTT map for true POM (Raymarching implemented).
    - [ ] Implement UV shifting for texture parallax (currently only darkening is refined).
- [ ] **Tuning**:
    - [ ] Adjust particle count, speed, and lifetime.
    - [ ] Tune RTT resolution and coverage size (currently 50m).
    - [ ] Adjust darkening intensity.
- [x] **Optimization**:
    - [x] **Implemented Persistent Buffer**: No longer rebuilds geometry every frame. Uses constant-cost texture updates.

## Comparison with Reference Paper ("Real-time Snow Deformation")

Based on the thesis "Real-time Snow Deformation" (Hanák, 2021), which implements a technique similar to *Horizon Zero Dawn: The Frozen Wilds*, here is a detailed comparison with our current OpenMW implementation.

### 1. Architecture: Persistent vs. Transient
*   **Reference (Paper)**: Uses a **persistent accumulation buffer** (ping-pong textures).
*   **Current OpenMW**: **MATCHED**. We now use a double-buffered (Ping-Pong) system.
    *   **Mechanism**: `mAccumulationMap[2]` stores the state. `mUpdateCamera` reads `ReadBuffer`, applies offset/decay/new-input, and writes to `WriteBuffer`.
    *   **Benefits**: Infinite footprints, snow accumulation, and constant performance.

### 2. Input Method: Depth Projection vs. Splatting
*   **Reference (Paper)**: **Depth Projection**.
*   **Current OpenMW**: **MATCHED**. We now use Depth Projection.
    *   **Mechanism**: `mDepthCamera` renders all actors (Mask_Actor | Mask_Player) from below into `mObjectMaskMap`. The update shader uses this mask to apply deformation.
    *   **Benefits**: Automatic support for all dynamic objects.

### 3. Visuals: Normal Reconstruction
*   **Reference (Paper)**: **Explicit Normal Reconstruction**.
*   **Current OpenMW**: **MATCHED**. We now implement Normal Reconstruction in `terrain.frag`.
    *   **Mechanism**: We calculate the gradient of the `snowDeformationMap` using 3 samples (center, right, up) to determine the slope, then perturb the view-space normal.
    *   **Result**: Footprint edges catch light and self-shadow correctly.

### 4. Geometry: Tessellation vs. Vertex Displacement
*   **Reference (Paper)**: **Hardware Tessellation**.
    *   **Mechanism**: Uses Hull/Domain shaders to dynamically subdivide the terrain mesh near the camera.
    *   **Result**: Smooth, high-res silhouettes for footprints.
*   **Current OpenMW**: **Vertex Displacement (VS)**.
    *   **Mechanism**: Displaces existing terrain vertices in `terrain.vert`.
    *   **Result**: Dependent on base terrain resolution. Footprints may look jagged if the terrain grid is coarse. (Note: We are mitigating this with POM in the fragment shader).

### 5. Rim/Edge Effect
*   **Reference (Paper)**: **Cubic Remapping**.
    *   **Mechanism**: Uses a specific cubic function (`f(x) = -3.47x^3 + ...`) to depress undeformed snow slightly, creating a raised "rim" around the deformation.
*   **Current OpenMW**: **Texture Alpha**.
    *   **Mechanism**: We rely on the footprint texture having a specific gradient.

## Summary of Missing Features
1.  **Persistent Accumulation**: Critical for performance and "snow filling" effects.
2.  **Depth Projection**: Would allow all actors/objects to deform snow without manual coding per-actor.
3.  **Normal Reconstruction**: Essential for visual quality (lighting).
4.  **Compute Shaders**: The paper relies on them. We may need to implement this using standard Fragment Shaders (Ping-Pong RTT) if Compute Shaders are not viable in the current pipeline.

