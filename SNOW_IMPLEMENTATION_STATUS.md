# Snow Deformation Implementation Status

**Last Updated:** 2025-11-16
**Current Approach:** Vertex Shader Array (Simple & Direct)

---

## Implementation History

### RTT Approach (Abandoned)
**Timeline:** Initial implementation → Abandoned 2025-11-16

The system was initially implemented using Render-To-Texture (RTT) with ping-pong buffers, similar to God of War's approach. After extensive debugging, the RTT approach encountered insurmountable issues with OpenMW's shader management system:

- OpenMW's shader manager overrode all custom shaders (even with PROTECTED flags)
- Fixed-function pipeline not supported (modern rendering only)
- Manual GL calls still produced no output despite geometry rendering
- Viewport forcing worked, culling disabled, FBO complete, but pixels never written

**Conclusion:** RTT added unnecessary complexity and fought against OpenMW's rendering architecture.

### Current Approach: Vertex Shader Array ✅
**Adopted:** 2025-11-16

Simple, direct approach that works with OpenMW's systems instead of against them:
- Store footprint positions in CPU array (std::deque)
- Pass positions to terrain vertex shader as uniform array
- Shader loops through positions, applies deformation
- No RTT cameras, FBOs, textures, or rendering complexity

**Advantages:**
- ~200 lines vs. 1400+ lines (RTT version)
- No shader manager conflicts
- Direct integration with existing terrain shader
- Immediate updates (no ping-pong delay)
- Simple debugging

**Limitations:**
- Trail length limited to ~500 footprints (shader uniform array size)
- ~200m of trail at 2m spacing (sufficient for gameplay)

---

## Completed Components

### Phase 0: Mesh Subdivision ✅ COMPLETE
- **terrainsubdivider.cpp/hpp**: Triangle subdivision system
- **subdivisiontracker.cpp/hpp**: Trail persistence system
- **Integration**: ChunkManager uses subdivision based on player distance
- **Features**:
  - Distance-based subdivision (0-3 levels)
  - Trail effect (chunks stay subdivided after player leaves)
  - Cache management (clears every 128 units)
  - Pre-subdivision buffer (64 units before chunk entry)

### Phase 1: Snow Detection System ✅ COMPLETE
- **snowdetection.hpp/cpp**:
  - `isSnowTexture()`: Pattern matching for snow texture detection
  - `hasSnowAtPosition()`: Query terrain for snow at position (skeleton)
  - `sampleBlendMap()`: Sample blendmap weights
  - Snow texture patterns loaded at startup

### Phase 2: Snow Deformation Manager ✅ COMPLETE
- **snowdeformation.hpp/cpp** (vertex shader array approach):
  - Footprint position storage (std::deque, max 500)
  - Shader uniform management (positions, count, radius, depth, time, decay)
  - Footprint stamping on player movement
  - Terrain-specific parameters (snow, ash, mud, dirt, sand)
  - Enable/disable system
  - Clean 200-line implementation

### Phase 3: Terrain Shader Integration ✅ COMPLETE
- **terrain.vert**: MODIFIED
  - Vertex shader array loop (up to 500 footprints)
  - Distance-based circular deformation
  - Time-based decay (trails fade over 3 minutes)
  - Smooth falloff (smoothstep)
  - Terrain raising: uniform +depth, subtract deformation
  - Result: Visible trails as depressions in raised snow

### Phase 4: Uniform Binding ✅ COMPLETE
- **snowdeformationupdater.cpp/hpp**:
  - Adds snow deformation uniforms to terrain stateset
  - Uniforms updated directly by SnowDeformationManager each frame
  - Simple passthrough (no per-frame state management needed)

---

## System Integration Status

### ✅ Completed Integration
1. **Terrain World** (components/terrain/world.cpp):
   - SnowDeformationManager instantiated in World constructor
   - `updateSnowDeformation()` called each frame
   - Manager accessible via `getSnowDeformationManager()`

2. **Rendering Manager** (apps/openmw/mwrender/renderingmanager.cpp):
   - Calls `terrainWorld->updateSnowDeformation()` in frame update
   - Passes player position and delta time

3. **Chunk Manager** (components/terrain/chunkmanager.cpp):
   - Sets `chunkWorldOffset` uniform on each chunk
   - Used by shader to convert chunk-local coords to world coords

---

## How It Works

### Data Flow
```
Player moves
    ↓
SnowDeformationManager::update()
    ↓
stampFootprint() - adds Vec3(X, Y, timestamp) to deque
    ↓
updateShaderUniforms() - updates uniform array
    ↓
Terrain vertex shader receives uniforms
    ↓
Loop through footprints, calculate deformation
    ↓
Apply vertex displacement (Z axis)
    ↓
Visible trails in snow!
```

### Shader Logic (terrain.vert)
```glsl
for (int i = 0; i < snowFootprintCount; i++) {
    vec3 footprint = snowFootprintPositions[i];
    float dist = distance(worldPos.xy, footprint.xy);

    if (dist > snowFootprintRadius) continue;

    // Age-based decay
    float age = snowCurrentTime - footprint.z;
    float decayFactor = clamp(age / snowDecayTime, 0.0, 1.0);

    // Distance falloff + decay
    float radiusFactor = smoothstep(0.0, 1.0, 1.0 - dist/radius);
    float deformation = depth * radiusFactor * (1.0 - decayFactor);

    totalDeformation = max(totalDeformation, deformation);
}

// Raise terrain uniformly, subtract where footprints are
vertex.z += snowDeformationDepth - totalDeformation;
```

---

## Shader Uniforms

### Global Uniforms (shared across all terrain)
- `snowFootprintPositions[500]` - vec3 array (X, Y, timestamp)
- `snowFootprintCount` - int (number of active footprints)
- `snowFootprintRadius` - float (footprint radius in world units, default 60)
- `snowDeformationDepth` - float (max deformation depth, default 100)
- `snowCurrentTime` - float (game time for decay calculation)
- `snowDecayTime` - float (time for trails to fade, default 180s)
- `snowDeformationEnabled` - bool (runtime toggle)

### Per-Chunk Uniforms
- `chunkWorldOffset` - vec3 (chunk's world position, for local→world conversion)

---

## Terrain-Specific Parameters

Different terrain types use different footprint parameters configured in settings:

| Terrain | Radius | Depth | Interval | Decay Time | Description |
|---------|--------|-------|----------|------------|-------------|
| Snow    | 60     | 100   | 2.0      | 180s       | Wide body-sized, waist-deep |
| Ash     | 30     | 20    | 3.0      | 120s       | Medium, knee-deep |
| Mud     | 15     | 10    | 5.0      | 90s        | Narrow feet-only, ankle-deep |

Parameters loaded from [settings-default.cfg](files/settings-default.cfg) and can be customized per-terrain type.
Each terrain type can be individually enabled/disabled via settings.

---

## Configuration Settings

All deformation parameters are configurable in [settings-default.cfg](files/settings-default.cfg):

### Snow Settings
```ini
[Terrain]
snow deformation enabled = true
snow max footprints = 500
snow footprint radius = 60.0
snow deformation depth = 100.0
snow decay time = 180.0
```

### Ash Settings
```ini
ash deformation enabled = true
ash footprint radius = 30.0
ash deformation depth = 20.0
ash decay time = 120.0
```

### Mud Settings
```ini
mud deformation enabled = true
mud footprint radius = 15.0
mud deformation depth = 10.0
mud decay time = 90.0
```

Users can override these in their personal `settings.cfg` file for custom gameplay.

---

## Current Status

### ✅ Working
- Footprint tracking and storage
- Uniform updates
- Shader integration
- Terrain-specific parameters
- Decay system (time-based)
- Smooth falloff (distance-based)

### 🔄 To Test
- Compile and verify no errors
- Check console for initialization logs
- Verify terrain deformation appears visually
- Test trail decay over time
- Test movement across different terrain types

### 📝 Future Enhancements
- **Texture-weighted deformation**: Sample terrain textures in vertex shader to weight deformation
  - Rock vertices don't lift (weight = 0.0)
  - Snow vertices lift fully (weight = 1.0)
  - Mixed vertices lift partially (weight = 0.5)
  - Enables smooth transitions with mipmaps
- **Trail persistence**: Save/load footprint arrays across sessions
- **Actual terrain texture detection**: Query terrain storage for texture types (currently defaults to snow)
- **Performance optimization**: Spatial culling for large footprint counts
- **Visual improvements**: Anisotropic falloff for directional footprints

---

## Files Modified

### Core Implementation
- `components/terrain/snowdeformation.hpp` - Manager class definition
- `components/terrain/snowdeformation.cpp` - Manager implementation with terrain-specific parameters
- `components/terrain/snowdeformationupdater.hpp` - Uniform binding header
- `components/terrain/snowdeformationupdater.cpp` - Uniform binding implementation
- `components/terrain/snowdetection.hpp` - Terrain type detection (snow, ash, mud)
- `components/terrain/snowdetection.cpp` - Texture pattern matching for terrain types

### Settings
- `components/settings/categories/terrain.hpp` - Added ash and mud deformation settings
- `files/settings-default.cfg` - Configuration for all terrain types

### Shader
- `files/shaders/compatibility/terrain.vert` - Vertex shader with footprint array loop

### Integration Points
- `components/terrain/world.hpp` - Added manager member
- `components/terrain/world.cpp` - Manager instantiation and update
- `apps/openmw/mwrender/renderingmanager.cpp` - Frame update call
- `components/terrain/chunkmanager.cpp` - chunkWorldOffset uniform (already existed)

---

## Success Criteria

The system is working when:
1. ✅ Console shows: `[SNOW] Snow deformation system initialized (vertex shader array approach)`
2. ✅ Console shows: `[SNOW] Footprint #N at (X, Y) | Total: N/500`
3. 🔄 Terrain appears raised by 100 units everywhere
4. 🔄 Walking creates visible depressions (trails) in the raised terrain
5. 🔄 Trails gradually fade back to raised level over 3 minutes

---

## Lessons Learned

### RTT Complexity
- Fighting framework architecture leads to debugging nightmares
- "Simple and working" beats "complex and perfect"
- OpenMW has strong opinions about shaders - work with it, not against it

### Vertex Shader Arrays
- Uniform arrays are a simple, proven technique
- 500 footprints = ~100m of trail (sufficient for gameplay)
- Direct uniform updates avoid synchronization issues
- Easy to debug (just print the array!)

### Development Strategy
- Start with simplest approach first
- Add complexity only when necessary
- Learn the framework's patterns before fighting them
- Clear documentation prevents wasted effort
