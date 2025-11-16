# Snow Deformation System - Comprehensive Audit Report

**Date**: 2025-11-16
**System Version**: Current implementation
**Status**: Functional but incomplete

---

## Executive Summary

The snow deformation system is **partially working** but has three critical missing features:

1. **Trail Decay System**: NOT IMPLEMENTED - Footprints are permanent
2. **Character Floating**: PARTIALLY ADDRESSED - Shader raises terrain but doesn't handle physics
3. **Stomping System**: WORKING - Creates continuous trail every 2 units

---

## Issue 1: Trail Decay System

### Current Status: ❌ NOT IMPLEMENTED

### What Should Happen
According to the implementation plan (lines 840-849 in SNOW_DEFORMATION_IMPLEMENTATION_PLAN.md):
- Trails should gradually fade over `snow decay time = 120.0` seconds (2 minutes)
- Decay curve types: linear, exponential, or smooth
- Can be toggled with `snow decay enabled = true`

### What Actually Happens
**Footprints are permanent.** Once stamped, they never fade.

### Evidence

#### 1. **Deformation Texture Stores Age Data**
`components/terrain/snowdeformation.cpp:143-144`:
```cpp
data[idx + 1] = 0.0f;   // G = age
```

The green channel is allocated for age tracking, BUT:
- It's initialized to 0.0
- It's updated when new footprints are stamped (line 287 in fragment shader)
- **It's never read or used for decay**

#### 2. **Footprint Shader Updates Age**
`snow_footprint.frag` (lines 286-288 in snowdeformation.cpp):
```glsl
// Update age where new footprint is stamped
float age = (influence > 0.01) ? currentTime : prevAge;
gl_FragColor = vec4(newDepth, age, 0.0, 1.0);
```

Age is set to `currentTime` where footprints are fresh, preserving `prevAge` elsewhere.

#### 3. **TODO Comment Confirms Missing Implementation**
`components/terrain/snowdeformation.cpp:399`:
```cpp
// TODO: Apply decay over time
```

### Why It's Missing

The implementation plan includes a complete decay shader design (Phase 5):
- **File**: `snow_decay.vert/frag` (NOT CREATED)
- **Purpose**: Reduce deformation depth based on time elapsed
- **Trigger**: Run periodically (every 0.5s)

**These shaders were never implemented.**

### How Decay Should Work

#### Design from Implementation Plan (Phase 5)

**Decay Shader (`snow_decay.frag`)**:
```glsl
uniform sampler2D currentDeformation;
uniform float currentTime;
uniform float decayRate;  // Units per second
uniform float deltaTime;   // Time since last decay pass

void main() {
    vec4 deform = texture2D(currentDeformation, texUV);
    float depth = deform.r;
    float age = deform.g;

    // Calculate how long this snow has been deformed
    float timeDeformed = currentTime - age;

    // Linear decay: depth reduces over time
    float decayAmount = decayRate * deltaTime;
    float newDepth = max(0.0, depth - decayAmount);

    gl_FragColor = vec4(newDepth, age, 0.0, 1.0);
}
```

**Trigger in C++** (`snowdeformation.cpp::update()`):
```cpp
void SnowDeformationManager::update(float dt, const osg::Vec3f& playerPos) {
    // ... existing code ...

    // Run decay every 0.5 seconds
    mDecayTimer += dt;
    if (mDecayTimer > 0.5f) {
        applyDecay(dt);
        mDecayTimer = 0.0f;
    }
}

void SnowDeformationManager::applyDecay(float dt) {
    // Swap ping-pong buffers
    int prevIndex = mCurrentTextureIndex;
    mCurrentTextureIndex = 1 - mCurrentTextureIndex;

    // Bind previous texture as input
    mDecayStateSet->setTextureAttributeAndModes(0,
        mDeformationTexture[prevIndex].get(), osg::StateAttribute::ON);

    // Attach current as render target
    mRTTCamera->attach(osg::Camera::COLOR_BUFFER,
        mDeformationTexture[mCurrentTextureIndex].get());

    // Update decay uniforms
    mDecayStateSet->getUniform("currentTime")->set(mCurrentTime);
    mDecayStateSet->getUniform("deltaTime")->set(dt);

    // Enable decay pass
    mDecayGroup->setNodeMask(~0u);
}
```

### Alternative Simpler Approach: Decay in Footprint Shader

Instead of a separate decay pass, we could modify the footprint shader to decay ALL pixels:

**Modified `snow_footprint.frag`**:
```glsl
uniform float decayRate;  // Units per second (e.g., 8.0 / 120.0 = 0.0667)
uniform float deltaTime;   // Time since last frame

void main() {
    vec4 prevDeform = texture2D(previousDeformation, texUV);
    float prevDepth = prevDeform.r;
    float prevAge = prevDeform.g;

    // DECAY: Reduce all deformation by a small amount each frame
    float decayedDepth = max(0.0, prevDepth - decayRate * deltaTime);

    // ... existing footprint stamping code ...

    // New footprint takes max of decayed depth and new stamp
    float newDepth = max(decayedDepth, influence * deformationDepth);

    // ... rest of shader ...
}
```

This would decay snow every frame without needing a separate shader.

### Recommended Fix: Option 2 (Simpler)

**Modify `snowdeformation.cpp:255-290` (footprint fragment shader)**:

Add decay to the footprint shader so it runs every frame:

```cpp
std::string fragSource = R"(
    #version 120
    uniform sampler2D previousDeformation;
    uniform vec2 deformationCenter;
    uniform float deformationRadius;
    uniform vec2 footprintCenter;
    uniform float footprintRadius;
    uniform float deformationDepth;
    uniform float currentTime;
    uniform float deltaTime;        // NEW: Time since last frame
    uniform float decayRate;        // NEW: Depth lost per second
    varying vec2 texUV;

    void main()
    {
        vec4 prevDeform = texture2D(previousDeformation, texUV);
        float prevDepth = prevDeform.r;
        float prevAge = prevDeform.g;

        // DECAY: Gradually reduce all deformation
        float decayedDepth = max(0.0, prevDepth - decayRate * deltaTime);

        // Convert UV to world position
        vec2 worldPos = deformationCenter + (texUV - 0.5) * 2.0 * deformationRadius;

        // Calculate footprint influence
        float dist = length(worldPos - footprintCenter);
        float influence = 1.0 - smoothstep(footprintRadius * 0.5, footprintRadius, dist);

        // New depth is max of decayed depth or new footprint
        float newDepth = max(decayedDepth, influence * deformationDepth);

        // Update age where new footprint applied
        float age = (influence > 0.01) ? currentTime : prevAge;

        gl_FragColor = vec4(newDepth, age, 0.0, 1.0);
    }
)";
```

**Add uniforms in `setupFootprintStamping()`** (after line 307):
```cpp
mFootprintStateSet->addUniform(new osg::Uniform("deltaTime", 0.0f));
mFootprintStateSet->addUniform(new osg::Uniform("decayRate", 0.0667f)); // 8 units / 120 seconds
```

**Update uniforms in `stampFootprint()`** (after line 515):
```cpp
osg::Uniform* deltaTimeUniform = mFootprintStateSet->getUniform("deltaTime");
if (deltaTimeUniform)
    deltaTimeUniform->set(dt);  // Pass from update() function
```

**Change `stampFootprint()` to receive dt**:
```cpp
void stampFootprint(const osg::Vec3f& position, float dt);
```

**Update call in `update()`** (line 354):
```cpp
stampFootprint(playerPos, dt);
```

### Why This Works Better

1. **Simple**: No separate decay shader or render pass
2. **Efficient**: Decay happens during footprint stamping (already running)
3. **Configurable**: Just change `decayRate` to adjust decay speed
4. **Works even when stationary**: Footprint shader runs every time player moves, which is frequent

---

## Issue 2: Character Floating

### Current Status: ⚠️ PARTIALLY ADDRESSED

### The Problem

When the terrain is deformed (creating a depression), the **visual mesh** goes down but the **collision mesh** doesn't. This causes the character to appear to float above the depression.

### Current "Solution" in Shader

`terrain.vert:165-183`:
```glsl
// FINAL IMPLEMENTATION: Raise terrain everywhere, then subtract deformation
if (snowDeformationEnabled)
{
    vec3 worldPos = vertex.xyz + chunkWorldOffset;
    vec2 relativePos = worldPos.xy - snowDeformationCenter;
    vec2 deformUV = (relativePos / snowDeformationRadius) * 0.5 + 0.5;

    float deformationDepth = texture2D(snowDeformationMap, deformUV).r;

    // Raise all terrain by max deformation depth (8 units default)
    // Then subtract actual deformation - creates depression where player walks
    const float maxDeformationDepth = 8.0;
    vertex.z += maxDeformationDepth - deformationDepth;

    // This means:
    // - Where no deformation: vertex.z += 8.0 (terrain raised)
    // - Where full deformation (8 units): vertex.z += 8.0 - 8.0 = 0 (original height)
    // - Character walks on raised surface, depressions are relative to raised level
}
```

### What This Does

- **Raises ALL terrain by 8 units**
- **Subtracts deformation** (0-8 units)
- Net effect: Terrain at original height where fully deformed, +8 units where untouched

### Why This Doesn't Fully Solve the Problem

1. **Collision mesh is NOT raised** - Only visual mesh changes
2. **Character still walks on original collision mesh** - Will still appear to float
3. **Requires collision system integration** - Physics needs to know about the raised terrain

### The Real Issue: OpenMW Collision System

According to the user's description:
> Because collisions are still quite a mess in Morrowind, I think a simple way to handle that is to raise all the vertices with snow on them, and dig by the same amount under the character's feet.

The user acknowledges that **modifying collision meshes is complex** in OpenMW/Morrowind.

### Proposed Solution: Texture-Aware Vertex Raising

The user suggests:
> Vertices with **no snow** should not be brought up at all
> Vertices with **snow and grass** should be brought up a bit
> Vertices with **pure snow** should be brought up fully

This requires:
1. **Sampling terrain texture layers at each vertex**
2. **Calculating snow blend weight** (0-1)
3. **Scaling the raise amount** by snow weight

### Implementation: Texture-Based Raising

**Problem**: The terrain shader doesn't currently have access to blend weights.

**Solution 1: Pass blend weight as vertex attribute**

Modify terrain mesh generation to include snow weight as a vertex color or extra attribute.

**Components/terrain/chunkmanager.cpp** (or wherever vertices are generated):
```cpp
// When creating vertices, sample blendmap for snow weight
for (each vertex) {
    osg::Vec2f uv = getVertexUV(vertex);
    float snowWeight = sampleSnowBlendMap(cellX, cellY, uv);

    // Store in vertex color alpha channel (or extra attribute)
    vertexColor.a() = snowWeight;  // 0.0 = no snow, 1.0 = pure snow
}
```

**Shader modification (terrain.vert:165-183)**:
```glsl
if (snowDeformationEnabled)
{
    // Get snow blend weight from vertex attribute
    float snowWeight = passColor.a;  // Assuming stored in vertex color alpha

    // Calculate deformation as before
    vec3 worldPos = vertex.xyz + chunkWorldOffset;
    vec2 relativePos = worldPos.xy - snowDeformationCenter;
    vec2 deformUV = (relativePos / snowDeformationRadius) * 0.5 + 0.5;
    float deformationDepth = texture2D(snowDeformationMap, deformUV).r;

    const float maxDeformationDepth = 8.0;

    // Raise terrain by (maxDepth * snowWeight) - deformation
    // Pure snow (snowWeight=1.0): Raises by 8, subtracts 0-8 deformation
    // No snow (snowWeight=0.0): Raises by 0, no effect
    // Mixed (snowWeight=0.5): Raises by 4, subtracts 0-4 deformation
    float raiseAmount = maxDeformationDepth * snowWeight;
    vertex.z += raiseAmount - deformationDepth;
}
```

**Solution 2: Sample blendmap in shader (more expensive)**

Pass blendmap textures to shader and sample them per-vertex.

**Pros**: No mesh modification needed
**Cons**: Multiple texture lookups per vertex, very expensive

### Recommended Approach

**Short-term (current implementation)**: Keep the current raise-all approach
- Works visually
- Character appears to sink into depressions (even if floating slightly)
- Simple and performant

**Long-term (proper fix)**:
1. Add snow blend weight as vertex attribute during terrain generation
2. Modify shader to scale raise amount by snow weight
3. Still won't fix collision, but will make transitions look better

**Collision Fix (complex, separate project)**:
- Modify Bullet physics terrain generation to also raise vertices
- Synchronize collision mesh with visual mesh
- Way beyond scope of this snow deformation system

### Why Collision is Hard in OpenMW

OpenMW uses Bullet Physics with static triangle meshes for terrain collision. These are:
1. Generated once when terrain loads
2. NOT updated when shaders modify vertices
3. Completely separate from rendering system

To fix collision properly, you'd need to:
1. Hook into terrain collision mesh generation
2. Apply the same vertex raising logic
3. Update Bullet's collision data dynamically
4. Handle performance (collision updates are expensive)

This is a **major undertaking** beyond the snow deformation system.

---

## Issue 3: Stomping System - What Is It Trying To Do?

### Current Status: ✅ WORKING AS DESIGNED

### What "Stomping" Means

The system creates a **continuous trail** of footprints as the player moves.

### How It Works

**Key Parameter**: `mFootprintInterval = 2.0f` (line 31 in snowdeformation.cpp)

**Logic in `update()`** (lines 349-357):
```cpp
mTimeSinceLastFootprint += dt;

float distanceMoved = (playerPos - mLastFootprintPos).length();
if (distanceMoved > mFootprintInterval || mTimeSinceLastFootprint > 0.5f)
{
    stampFootprint(playerPos);
    mLastFootprintPos = playerPos;
    mTimeSinceLastFootprint = 0.0f;
}
```

### Triggering Conditions

A new footprint is stamped when **EITHER**:
1. Player moves **2 units** from last footprint, OR
2. **0.5 seconds** have passed since last footprint

### Why Two Conditions?

1. **Distance check** (`distanceMoved > 2.0`):
   - Creates continuous trail when walking
   - Spacing: Every 2 units = very tight trail
   - Like God of War's snow trails (reference in comments)

2. **Time check** (`timeSinceLastFootprint > 0.5f`):
   - Ensures footprints even when stationary
   - Prevents "stuck" state if player barely moves
   - Fallback for edge cases

### What Gets Stamped

**Each footprint** (lines 279-284 in fragment shader):
```glsl
// Circular falloff: full depth at center, fades to zero at radius
float influence = 1.0 - smoothstep(footprintRadius * 0.5, footprintRadius, dist);

// Accumulate deformation (keep maximum depth)
float newDepth = max(prevDepth, influence * deformationDepth);
```

- **Shape**: Circular, smooth falloff
- **Size**: `mFootprintRadius = 24.0f` units
- **Depth**: `mDeformationDepth = 8.0f` units at center
- **Accumulation**: `max()` function means overlapping footprints don't get deeper

### Expected Visual Result

With current settings:
- **Footprint every 2 units** = 12 footprints per full rotation in a circle of radius 24 units
- **Overlapping footprints** = Continuous trail, not discrete steps
- **Smooth depressions** = Like walking through deep powder snow

### Why "Stomping" vs "Walking"?

The implementation plan mentions "Footprint Stamping System" (line 17) - "stamping" implies:
- Projecting player position into deformation texture
- Rendering a circular pattern at that position
- It's a **rendering technique**, not a gameplay mechanic

**Not** about player stomping their feet, but about **stamping images into a texture**.

### Is It Working?

**Yes**, according to implementation status (line 6):
```
✅ Footprints being stamped
```

The RTT camera is rendering footprints into the deformation texture successfully.

### Performance Characteristics

**Footprint stamping frequency**:
- Running at 60 FPS with character moving at 300 units/sec
- Footprint every 2 units = 150 footprints per second
- Each footprint triggers:
  - Ping-pong buffer swap
  - Full-screen quad render to 512x512 texture
  - Shader evaluation for ~262k pixels

**Potential issue**: Stamping 150 times/second might be overkill

**Optimization opportunity**: Increase `mFootprintInterval` to reduce frequency
- `mFootprintInterval = 10.0f` = 30 footprints/sec (still very frequent)
- `mFootprintInterval = 15.0f` (from plan) = 20 footprints/sec (reasonable)

Current value of `2.0f` seems like a testing artifact.

---

## Summary of Findings

| Issue | Status | Severity | Effort to Fix |
|-------|--------|----------|---------------|
| **Trail Decay** | ❌ Not implemented | HIGH | Medium (2-4 hours) |
| **Character Floating** | ⚠️ Partial workaround | MEDIUM | High (8+ hours for full fix) |
| **Stomping System** | ✅ Working | N/A | N/A |

---

## Recommended Action Plan

### Priority 1: Implement Trail Decay (2-4 hours)

**Why**: Critical for gameplay - permanent footprints are unrealistic

**Approach**: Modify footprint shader to include decay (simpler than separate pass)

**Steps**:
1. Add `deltaTime` and `decayRate` uniforms to footprint shader
2. Modify fragment shader to decay all pixels before stamping new footprint
3. Pass `dt` to `stampFootprint()` function
4. Test with different decay rates

**Files to modify**:
- `components/terrain/snowdeformation.cpp` (lines 255-323)

### Priority 2: Tune Footprint Interval (5 minutes)

**Why**: Performance optimization, currently stamping too frequently

**Approach**: Change constant

**Change**:
```cpp
// Line 31 in snowdeformation.cpp
mFootprintInterval(15.0f)  // Changed from 2.0f
```

### Priority 3: Document Collision Limitation (30 minutes)

**Why**: Set proper expectations, collision fix is beyond scope

**Approach**: Update documentation to explain:
- Current raise-all approach
- Why collision isn't fixed
- What would be needed for full fix
- Texture-based raising as future enhancement

### Priority 4: Improve Texture-Based Raising (4-6 hours, OPTIONAL)

**Why**: Better visual transitions between snow/non-snow terrain

**Approach**: Add snow blend weight to vertex attributes

**Requirements**:
- Modify terrain mesh generation
- Sample blendmaps during vertex creation
- Store in vertex attribute
- Modify shader to use weight

**Files to modify**:
- Terrain chunk generation code
- `terrain.vert` shader

---

## Technical Debt & Future Work

### Identified Issues

1. **No settings integration**: All parameters hardcoded
2. **Snow detection stubbed**: Currently enables everywhere
3. **No debug visualization controls**: Always on
4. **Hardcoded paths**: Test pattern saving, diagnostics
5. **Magic numbers**: Many constants should be configurable

### Future Enhancements

1. **Dynamic decay rate based on weather**: Faster restoration in wind/sun
2. **Depth-based decay**: Deeper footprints take longer to fade
3. **Normal map generation**: Make deformation affect lighting
4. **Particle effects**: Snow particles when walking
5. **Sound integration**: Footstep sounds change with depth

---

## Conclusion

The snow deformation system has a **solid foundation** but needs:
1. **Decay implementation** (critical)
2. **Parameter tuning** (trivial)
3. **Documentation of limitations** (important for expectations)

The collision/floating issue is **acknowledged but complex** - the current workaround is reasonable given OpenMW's architecture.

The stomping system is **working correctly** - it's creating continuous trails as designed.

**Estimated time to feature-complete**: 4-6 hours (decay + tuning + docs)
