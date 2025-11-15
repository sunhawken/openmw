# Snow Deformation System - Diagnostic Audit

## Problem

The snow deformation system appears fully integrated:
- ✅ Footprints being stamped
- ✅ Deformation texture created
- ✅ Uniforms bound and set correctly
- ✅ Shader compiles without errors
- ✅ Even a hardcoded 100-unit terrain drop added

**BUT: No visual deformation is appearing!**

## What I've Added - Comprehensive Shader Diagnostics

### Changes Made to `components/terrain/material.cpp`

Added extensive logging to verify what shader is actually being compiled and used:

1. **Shader Creation Verification**
   - Logs when the terrain shader program is created
   - Reports if program creation fails

2. **Source Code Inspection**
   - Checks if the hardcoded `vertex.y -= 100.0;` is in the compiled shader
   - Checks if snow deformation uniforms are present
   - Logs a snippet of the code around the vertex modification
   - **Writes the full vertex shader source to `/tmp/openmw_terrain_vertex_shader_debug.glsl`**

3. **Shader Type Verification**
   - Lists all shaders in the program (vertex, fragment, etc.)

### What to Look For

When you run the game, you'll see console messages like:

```
[TERRAIN SHADER] Program created successfully
[TERRAIN SHADER] Snow deformation define: 1
[TERRAIN SHADER] Program has 2 shaders
[TERRAIN SHADER] Shader 0 type: VERTEX
[TERRAIN SHADER] Vertex shader length: XXXX
[TERRAIN SHADER] ✓ Hardcoded 100-unit drop FOUND in shader source!
[TERRAIN SHADER] ✓ Snow deformation uniforms FOUND in shader
[TERRAIN SHADER] Full vertex shader source written to /tmp/openmw_terrain_vertex_shader_debug.glsl
```

## Possible Causes of Invisible Deformation

Based on the symptoms, here are the most likely issues:

### 1. **Shader Cache Problem** (MOST LIKELY)
OpenMW or OSG might be caching the old shader.

**Solution:**
- Delete shader cache before running
- Look for cache in user data directory
- Try running with `--clean` flag if available

### 2. **Vertex Position Modified After Shader**
The shader might run correctly, but something else (e.g., physics, collision mesh) could be overriding the visual mesh position.

**Check:** Are you modifying the visual mesh or a physics mesh?

### 3. **Coordinate System Issue**
The `vertex.y -= 100.0` might be in the wrong coordinate space.

**Check:** Is Y the vertical axis in OpenMW's world space at this point in the shader?

### 4. **Shader Not Applied to All Terrain**
The shader might only apply to certain terrain passes or LOD levels.

**Check:** Does terrain have multiple render passes? Are you only modifying one?

### 5. **View Frustum Culling**
If vertices move too far, they might be culled.

**Check:** Try `vertex.y += 100.0` instead of `vertex.y -= 100.0` to make terrain go up.

### 6. **Preprocessor Issue with @defines**
The `@snowDeformation` define might not be working as expected.

**Check:** Look at the generated shader in `/tmp/openmw_terrain_vertex_shader_debug.glsl`

## Testing Instructions

### Step 1: Build the Code

```bash
cd build
cmake --build . --config RelWithDebInfo
```

### Step 2: Clear Any Shader Cache

```bash
# Find and delete shader cache (location varies by platform)
# Common locations:
# Linux: ~/.local/share/openmw/cache/
# Windows: C:\Users\[User]\Documents\My Games\OpenMW\cache\
# Look for files like "shadercache.bin" or similar
```

### Step 3: Run and Check Logs

```bash
cd RelWithDebInfo
./openmw 2>&1 | grep -E "\[TERRAIN SHADER\]|\[SNOW\]"
```

### Step 4: Examine the Generated Shader

```bash
cat /tmp/openmw_terrain_vertex_shader_debug.glsl
```

Look for:
- Is `vertex.y -= 100.0;` present?
- Are the uniforms declared?
- Is there any unexpected preprocessing?

### Step 5: Check What You See

#### If you see `✓ Hardcoded 100-unit drop FOUND`:
The shader source is correct. The problem is:
- Shader cache (old version still running)
- Coordinate system issue
- Something overriding the mesh position
- View frustum culling

#### If you see `✗ Hardcoded 100-unit drop NOT FOUND`:
The shader source is wrong. The problem is:
- Wrong shader file being loaded
- Preprocessor stripping out the code
- Different shader variant for terrain

#### If you see `FAILED to create terrain shader program`:
Shader compilation is broken. Check:
- GLSL syntax errors
- Missing shader files
- Shader manager configuration

## Explicit Verification Steps

### Verify Shader is Actually Running

Add this temporary code to terrain.vert at line 48 to make it IMPOSSIBLE to miss:

```glsl
// DIAGNOSTIC: Output red color if shader runs
gl_FrontColor = vec4(1.0, 0.0, 0.0, 1.0);  // Entire terrain should turn RED

// DIAGNOSTIC: Massive displacement
vertex.y -= 1000.0;  // 10x bigger - should be ~14 meters deep
```

If the terrain doesn't turn red and doesn't drop by 14 meters, **the shader is NOT running on the visible terrain**.

### Verify Coordinate Space

Check what coordinate space `vertex` is in at line 42 of terrain.vert:

```glsl
vec4 vertex = gl_Vertex;  // Is this model space, world space, or view space?
```

According to the shader, it should be model space (since it's later transformed by `modelToClip(vertex)`).

### Verify Terrain Uses Shaders

Check if terrain is using shader-based rendering or fixed-function rendering.

In `material.cpp:257`, there's a condition:
```cpp
if (useShaders)
```

Make sure this is `true`. If `useShaders` is false, terrain uses fixed-function pipeline and your shader code won't run.

## Next Steps Based on Results

### Scenario A: Logs show shader is correct, but no visual change
**Diagnosis:** Shader cache or coordinate system issue

**Actions:**
1. Clear shader cache completely
2. Restart the game
3. Try `vertex.y += 1000.0` (go up instead of down)
4. Try `vertex.x += 1000.0` (sideways displacement)
5. Add red color diagnostic

### Scenario B: Logs show shader is missing the deformation code
**Diagnosis:** Wrong shader variant or preprocessing issue

**Actions:**
1. Check `/tmp/openmw_terrain_vertex_shader_debug.glsl` manually
2. Verify the file `files/shaders/compatibility/terrain.vert` is correct
3. Check if shader manager loads from a different path
4. Verify `@define` preprocessing works correctly

### Scenario C: No logs appear at all
**Diagnosis:** Shader program never created or wrong code path

**Actions:**
1. Verify `useShaders` is true in material.cpp
2. Check if terrain uses a different material system
3. Look for other terrain shader creation code
4. Verify the logging code is actually compiled in

## Key Files for Manual Inspection

1. **Shader Source:** `/home/user/openmw-snow/files/shaders/compatibility/terrain.vert`
2. **Compiled Shader:** `/tmp/openmw_terrain_vertex_shader_debug.glsl` (generated at runtime)
3. **Material Setup:** `components/terrain/material.cpp:314`
4. **Uniform Binding:** `components/terrain/snowdeformationupdater.cpp:29`
5. **Deformation Manager:** `components/terrain/snowdeformation.cpp`

## Questions to Answer

These diagnostic logs will definitively answer:

1. ✓ **Is the terrain shader being created?** → Check for "Program created successfully"
2. ✓ **Is the hardcoded drop in the compiled shader?** → Check for "FOUND in shader source"
3. ✓ **Are the uniforms declared?** → Check for "uniforms FOUND"
4. ✓ **What does the actual compiled shader look like?** → Read `/tmp/openmw_terrain_vertex_shader_debug.glsl`
5. ? **Is the shader actually running on the GPU?** → Need to test with red color diagnostic
6. ? **Is the shader applied to all terrain or just some?** → Need to observe in-game
7. ? **Is there shader caching preventing updates?** → Need to clear cache and retry

## Summary

The diagnostic code will tell us **exactly** what shader the terrain is using. Once you run this and provide:

1. The console output (especially [TERRAIN SHADER] messages)
2. The contents of `/tmp/openmw_terrain_vertex_shader_debug.glsl`
3. What you visually see in the game

We'll know definitively what's wrong and how to fix it.

---

**Next Action:** Build, run, and report back with the diagnostic output!
