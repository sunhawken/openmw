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

### 2. RTT System (DONE)
*   **Class**: `SnowDeformationManager`
*   **Tech**:
    *   Creates a 2048x2048 `GL_RGBA16F` texture (`mDeformationMap`).
    *   Uses an Orthographic Camera (`mRTTCamera`) centered on the player.
    *   Renders footprints as soft quads into the texture every frame.
*   **Uniforms**: Passes `snowDeformationMap`, `snowRTTWorldOrigin`, and `snowRTTScale` to shaders.

### 3. Shaders (DONE - Basic)
*   **Vertex Shader** (`terrain.vert`):
    *   Replaced array-based loop with a single `texture2D(snowDeformationMap, uv)` sample.
    *   Lowers vertices based on the Red channel of the RTT map.
*   **Fragment Shader** (`terrain.frag`):
    *   Receives `vDeformationFactor` from vertex shader.
    *   **Darkening**: Multiplies diffuse color by `(1.0 - factor * 0.4)` to simulate wet/compressed snow.

## Remaining Tasks / Tracker

- [ ] **Verification**: Test in-game.
    - [ ] Verify footprints appear (RTT working).
    - [ ] Verify particles spawn.
    - [ ] Verify terrain deforms physically.
- [ ] **Parallax Occlusion Mapping (POM)**:
    - [x] Upgrade `terrain.frag` to use the RTT map for true POM (Raymarching implemented).
    - [ ] Implement UV shifting for texture parallax (currently only darkening is refined).
    - [ ] This will make the footprints look like 3D holes even if the mesh resolution is low.
- [ ] **Tuning**:
    - [ ] Adjust particle count, speed, and lifetime.
    - [ ] Tune RTT resolution and coverage size (currently 50m).
    - [ ] Adjust darkening intensity.
- [ ] **Optimization**:
    - [ ] Current RTT rebuilds geometry every frame. Consider updating only new footprints or using a scrolling shader approach if performance is an issue.
