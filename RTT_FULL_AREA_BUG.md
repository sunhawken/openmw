# RTT Full-Area Deformation Bug - Diagnostic Report

## Symptom
The entire RTT camera area is being depressed/deformed as a square, rather than just where the player/NPCs are standing.

## Observed Behavior

### In Wireframe Mode
- Vertices appear **NOT** deformed
- They look at the same altitude as before deformation
- This proves the **vertex shader is NOT displacing vertices down**

### In Regular Mode
- Deformation appears as a **square-shaped depression**
- The square matches the RTT camera zone
- This proves the **fragment shader POM is creating the visual effect**

## Root Cause Analysis

The vertex shader code in `terrain.vert:98-99` is:
```glsl
// Apply deformation: raise terrain by baseLift, then subtract where footprints are
vertex.z += baseLift * (1.0 - vDeformationFactor);
```

**This means:**
- When `vDeformationFactor = 0` (no deformation): `vertex.z += baseLift` → **RAISED**
- When `vDeformationFactor = 1` (full deformation): `vertex.z += 0` → **unchanged**

**The Problem:** The object mask texture (`mObjectMaskMap`) is **FULL WHITE** across the entire RTT area, making `vDeformationFactor = 1.0` everywhere.

## Why the Object Mask is Full White

### Depth Camera Configuration
Location: `snowdeformation.cpp:515-585`

```cpp
mDepthCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f)); // Clear to Black
mDepthCamera->setCullMask((1 << 3) | (1 << 4) | (1 << 10)); // Actor | Player | Object
```

The depth camera uses a **CullCallback** to manually traverse the scene and render actors.

### Shader Override
```cpp
dProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT,
    "void main() {\n"
    "  gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"  // Output WHITE for all pixels
    "}\n"));
```

**This is correct!** It should output white where geometry is rendered.

### Potential Causes

1. **Terrain is being rendered to the depth camera**
   - The CullCallback tries to skip "Terrain Root" but terrain might have a different name
   - Check actual terrain node names in the scene graph

2. **Sky/Background is being rendered**
   - Some background node might be matching the cull mask
   - The clear color is BLACK (0,0,0,0) so this shouldn't happen

3. **The entire player model is visible from below**
   - The camera looks UP from below the player
   - If the player's full body is in view, it would create a large silhouette
   - This would be correct behavior but might look like "full area"

4. **Camera frustum is wrong**
   - The projection might be too large or positioned incorrectly
   - Check: `playerPos.x() ± halfSize` alignment

## Diagnostic Steps

### Step 1: Check Object Mask Texture Content
Add debug visualization to see what the depth camera is actually rendering.

**In `terrain.frag`, add:**
```glsl
// DEBUG: Show object mask
vec2 debugUV = (passWorldPos.xy - snowRTTWorldOrigin.xy) / snowRTTScale + 0.5;
if (debugUV.x >= 0.0 && debugUV.x <= 1.0 && debugUV.y >= 0.0 && debugUV.y <= 1.0)
{
    float mask = texture2D(snowDeformationMap, debugUV).r;
    gl_FragData[0].rgb = mix(gl_FragData[0].rgb, vec3(0, 1, 0), 0.3); // Green tint = in RTT
    if (mask > 0.1) {
        gl_FragData[0].rgb = mix(gl_FragData[0].rgb, vec3(1, 0, 0), mask); // Red = deformation
    }
}
```

**Expected Result:**
- Green box = RTT coverage area
- Red dot/circle = Player position only
- **Current Result:** Entire green box is red

### Step 2: Check Terrain Node Name
The callback skips `child->getName() == "Terrain Root"`, but OpenMW terrain might use a different name.

**Add logging:**
```cpp
// In DepthCameraCullCallback::operator()
for (unsigned int i = 0; i < mRoot->getNumChildren(); ++i)
{
    osg::Node* child = mRoot->getChild(i);
    Log(Debug::Info) << "Depth camera sees node: " << child->getName();
    // ... rest of code
}
```

### Step 3: Check What's Being Rendered
Temporarily disable the terrain skip to see if that's the issue:

```cpp
// TEMPORARY: Comment out terrain skip
// if (child->getName() == "Terrain Root")
//     continue;
```

If the issue persists, terrain is NOT the problem.

### Step 4: Check Player Model Rendering
The depth camera should only see the player's **feet/legs**, not the entire body.

**Possible issues:**
- Player model extends too far (full body visible from below)
- LOD settings render full model
- Camera Z-range (2000 units) is too large

## Hypothesis

**Most Likely:** The player model's full body is being rendered as a silhouette, creating a large white area in the object mask.

**Why this creates a square depression:**
- The player body might be centered in the RTT view
- When rendered from below looking up, the body creates a large shape
- This shape gets sampled by the terrain as "deformation everywhere"

## Proposed Fixes

### Option 1: Only Render Player Feet (Recommended)
Modify the depth camera to use a custom node mask that only includes "feet" nodes or low-poly LODs.

```cpp
// Render only low LOD or specific body parts
mDepthCamera->setCullMask(MASK_PLAYER_FEET | MASK_NPC_FEET);
```

### Option 2: Reduce Camera Z-Range
The current range is 2000 units (~90 feet). This might capture too much of the player model.

```cpp
double depthRange = 200.0; // Only capture ~9 feet (enough for legs)
```

### Option 3: Use Depth Testing
Instead of binary white/black, use actual depth values to only mark pixels close to terrain.

```cpp
// In fragment shader:
float depth = gl_FragCoord.z; // 0 (near) to 1 (far)
if (depth < 0.1) { // Only mark pixels very close to camera
    gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);
} else {
    discard; // Don't mark distant pixels
}
```

## Next Actions

1. ✅ Revert the incorrect vertex shader change
2. ⬜ Add debug visualization to see object mask
3. ⬜ Log terrain node names
4. ⬜ Try reducing camera Z-range
5. ⬜ Implement depth-based filtering

## References
- `snowdeformation.cpp:515-585` - Depth camera setup
- `terrain.vert:98-99` - Vertex displacement logic
- `rtt_dom_particles.md:98-134` - Depth camera architecture

## Status
- **Issue:** Identified - Object mask is full white
- **Root Cause:** Unknown (investigating)
- **Priority:** HIGH - Blocks snow deformation functionality
