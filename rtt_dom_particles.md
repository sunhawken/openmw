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

## Comparison with Reference Paper ("Real-time Snow Deformation")

Based on the thesis "Real-time Snow Deformation" (Hanák, 2021), which implements a technique similar to *Horizon Zero Dawn: The Frozen Wilds*, here is a detailed comparison with our current OpenMW implementation.

### 1. Architecture: Persistent vs. Transient
*   **Reference (Paper)**: Uses a **persistent accumulation buffer** (ping-pong textures).
    *   **Mechanism**: A compute shader reads the previous frame's deformation, applies new deformation, and writes to the current frame.
    *   **Benefits**: Infinite footprints (old ones persist), snow accumulation (filling trails over time), and constant performance cost regardless of footprint count.
    *   **Sliding Window**: The buffer represents a window around the player. When the player moves, the window slides, and edge texels are reset.
*   **Current OpenMW**: Uses a **transient "rebuild-every-frame" approach**.
    *   **Mechanism**: We clear the RTT and redraw all active footprints (`mFootprints` list) every frame.
    *   **Limitations**: Linear performance cost (O(N) footprints), hard limit on max footprints, no automatic "filling" over time (we simulate decay by fading alpha, but it's not true accumulation).

### 2. Input Method: Depth Projection vs. Splatting
*   **Reference (Paper)**: **Depth Projection**.
    *   **Mechanism**: Renders "snow-affecting objects" (characters, animals) from *below* into an orthographic depth buffer.
    *   **Benefits**: Handles *any* geometry automatically. If a character falls face-first, their entire body shape deforms the snow. No need for specific "footprint" logic.
*   **Current OpenMW**: **Texture Splatting**.
    *   **Mechanism**: We manually track footfalls and "stamp" a quad with a specific texture (footprint sprite) at that location.
    *   **Limitations**: Only works for feet/tracked points. Doesn't handle dragging bodies, rolling, or complex shapes without manual work.

### 3. Visuals: Normal Reconstruction
*   **Reference (Paper)**: **Explicit Normal Reconstruction**.
    *   **Mechanism**: A compute shader calculates the gradient of the heightmap (using finite difference) to generate a normal map on the fly.
    *   **Result**: The edges of the deformation catch light (specular highlights), making them look like 3D geometry even on flat surfaces.
*   **Current OpenMW**: **None / Implicit**.
    *   **Mechanism**: We displace vertices and darken the color. We do *not* perturb the normal in the fragment shader based on the RTT.
    *   **Result**: Footprints look "flat" or "painted on" under dynamic lighting, especially when viewed from grazing angles.

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

