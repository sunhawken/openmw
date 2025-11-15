# Snow Deformation Implementation Status

## Completed Components

### Phase 0: Mesh Subdivision ✅ COMPLETE
- **terrainsubdivider.cpp/hpp**: Triangle subdivision system (existing)
- **subdivisiontracker.cpp/hpp**: Trail persistence system (existing)
- **Integration**: ChunkManager uses subdivision based on player distance
- **Features**:
  - Distance-based subdivision (0-3 levels)
  - Trail effect (chunks stay subdivided after player leaves)
  - Cache management (clears every 128 units)
  - Pre-subdivision buffer (64 units before chunk entry)

### Phase 1: Snow Detection System ✅ COMPLETE
- **snowdetection.hpp/cpp**: NEW
  - `isSnowTexture()`: Pattern matching for snow texture detection
  - `hasSnowAtPosition()`: Query terrain for snow at position (skeleton)
  - `sampleBlendMap()`: Sample blendmap weights
  - Snow texture patterns loaded at startup

### Phase 3: RTT & Deformation Texture ✅ COMPLETE
- **snowdeformation.hpp/cpp**: NEW
  - RTT camera setup (orthographic, top-down)
  - Ping-pong deformation textures (512x512 RGBA16F)
  - Footprint stamping system (skeleton)
  - Position tracking and camera following
  - Enable/disable system

- **Shaders**: NEW
  - `snow_footprint.vert`: Footprint stamping vertex shader
  - `snow_footprint.frag`: Footprint accumulation fragment shader
    - Ping-pong rendering
    - Circular falloff
    - Depth accumulation (max)
    - Age tracking for decay

### Phase 4: Terrain Shader Integration ✅ COMPLETE
- **terrain.vert**: MODIFIED
  - Added `@snowDeformation` shader define
  - Deformation texture sampling
  - Vertex displacement (downward Y)
  - World-space to texture UV conversion
  - Bounds checking for deformation area

## Remaining Work

### 1. Shader Manager Integration (HIGH PRIORITY)

The terrain shader needs to be compiled with the `@snowDeformation` define enabled.

**Location**: `components/terrain/material.cpp` or similar

**Required Changes**:
```cpp
// In terrain material setup:
shaderDefines["snowDeformation"] = "1";  // Enable snow deformation shader path
```

### 2. Uniform Binding (HIGH PRIORITY)

The terrain drawable needs to bind deformation texture uniforms each frame.

**Location**: `components/terrain/terraindrawable.cpp` or `material.cpp`

**Required Uniforms**:
```cpp
stateset->addUniform(new osg::Uniform("snowDeformationMap", textureUnit));
stateset->addUniform(new osg::Uniform("snowDeformationCenter", osg::Vec2f(x, z)));
stateset->addUniform(new osg::Uniform("snowDeformationRadius", radius));
stateset->addUniform(new osg::Uniform("snowDeformationEnabled", enabled));
```

### 3. Snow Deformation Manager Integration (HIGH PRIORITY)

The manager needs to be instantiated and updated from the rendering system.

**Option A**: Integrate into `World` class (components/terrain/world.cpp)
```cpp
// In World class:
std::unique_ptr<SnowDeformationManager> mSnowDeformationManager;

// In constructor:
mSnowDeformationManager = std::make_unique<SnowDeformationManager>(
    resourceSystem->getSceneManager(),
    mStorage,
    mTerrainRoot
);

// Add update method:
void World::updateSnowDeformation(float dt, const osg::Vec3f& playerPos)
{
    if (mSnowDeformationManager)
        mSnowDeformationManager->update(dt, playerPos);
}
```

**Option B**: Integrate into RenderingManager (apps/openmw/mwrender/renderingmanager.cpp)
- Create manager in RenderingManager constructor
- Call update() in each frame update
- Pass to terrain system for texture binding

### 4. Footprint Shader Setup (MEDIUM PRIORITY)

Complete the footprint stamping shader setup in snowdeformation.cpp:

```cpp
// In setupFootprintStamping():
osg::ref_ptr<osg::Program> program = new osg::Program;

// Load shaders
osg::Shader* vertShader = mSceneManager->getShaderManager()
    .getShader("snow_footprint.vert", {}, osg::Shader::VERTEX);
osg::Shader* fragShader = mSceneManager->getShaderManager()
    .getShader("snow_footprint.frag", {}, osg::Shader::FRAGMENT);

program->addShader(vertShader);
program->addShader(fragShader);
mFootprintStateSet->setAttributeAndModes(program, osg::StateAttribute::ON);

// Bind uniforms
mFootprintStateSet->addUniform(new osg::Uniform("previousDeformation", 0));
mFootprintStateSet->addUniform(new osg::Uniform("deformationCenter", mTextureCenter));
mFootprintStateSet->addUniform(new osg::Uniform("deformationRadius", mWorldTextureRadius));
// ... more uniforms
```

### 5. Ping-Pong Buffer Swap (MEDIUM PRIORITY)

Implement actual ping-pong rendering in stampFootprint():

```cpp
void SnowDeformationManager::stampFootprint(const osg::Vec3f& position)
{
    // Swap textures
    int prevIndex = mCurrentTextureIndex;
    mCurrentTextureIndex = 1 - mCurrentTextureIndex;

    // Bind previous texture as input (texture unit 0)
    mFootprintStateSet->setTextureAttributeAndModes(0,
        mDeformationTexture[prevIndex].get(),
        osg::StateAttribute::ON);

    // Attach current texture as render target
    mRTTCamera->detach(osg::Camera::COLOR_BUFFER);
    mRTTCamera->attach(osg::Camera::COLOR_BUFFER,
        mDeformationTexture[mCurrentTextureIndex].get());

    // Update uniforms
    auto centerUniform = mFootprintStateSet->getUniform("footprintCenter");
    centerUniform->set(osg::Vec2f(position.x(), position.z()));

    auto timeUniform = mFootprintStateSet->getUniform("currentTime");
    timeUniform->set(mCurrentTime);

    // Enable rendering for one frame
    mFootprintGroup->setNodeMask(~0u);

    // Disable after render (use callback or frame update)
}
```

### 6. Complete Snow Detection (MEDIUM PRIORITY)

Implement actual terrain layer querying in `hasSnowAtPosition()`:

**Requires**:
- Access to chunk layer information
- Blendmap sampling
- Snow texture pattern matching

**Current Status**: Returns `true` everywhere (testing mode)

### 7. Decay System (LOW PRIORITY)

Implement gradual snow restoration over time:

**Create**: `snow_decay.vert/frag` shaders
**Purpose**: Apply decay based on age stored in green channel
**Trigger**: Run decay shader periodically (every 0.5s)

### 8. Settings Integration (LOW PRIORITY)

Add settings to `files/settings-default.cfg`:

```ini
[Terrain]
snow deformation = true
snow deformation resolution = 512
snow deformation radius = 150.0
snow footprint radius = 24.0
snow footprint interval = 15.0
snow deformation depth = 8.0
```

Load in SnowDeformationManager constructor.

## Testing Strategy

### Phase 1: Verify Subdivision Works
- [x] Player movement creates subdivided chunks
- [x] Trail effect persists after player leaves
- [x] Cache clearing works correctly

### Phase 2: Verify Shader Compilation
- [ ] Terrain shader compiles with `@snowDeformation` define
- [ ] No shader errors in log
- [ ] Uniforms bind correctly

### Phase 3: Verify Deformation Rendering
- [ ] RTT camera renders correctly
- [ ] Deformation texture updates when player moves
- [ ] Footprints accumulate in texture

### Phase 4: Verify Visual Result
- [ ] Terrain vertices displace downward where footprints exist
- [ ] Deformation follows player smoothly
- [ ] No artifacts at texture boundaries

### Phase 5: Performance Testing
- [ ] 60 FPS maintained with many subdivided chunks
- [ ] Memory usage stays under 250MB for deformation
- [ ] No stuttering when stamping footprints

## File Structure

### New Files
```
components/terrain/
  snowdetection.hpp/cpp       ✅ Created
  snowdeformation.hpp/cpp     ✅ Created

files/shaders/compatibility/
  snow_footprint.vert         ✅ Created
  snow_footprint.frag         ✅ Created
  snow_decay.vert            ⬜ TODO (optional)
  snow_decay.frag            ⬜ TODO (optional)
```

### Modified Files
```
components/terrain/
  terrain.vert               ✅ Modified (deformation sampling)

components/
  CMakeLists.txt             ✅ Modified (added new terrain files)

PENDING MODIFICATIONS:
  components/terrain/material.cpp     ⬜ Need to add shader define
  components/terrain/world.cpp        ⬜ Need to integrate manager
  apps/openmw/mwrender/renderingmanager.cpp  ⬜ Need to call update
```

## Next Steps (Priority Order)

1. **Add shader define** for `@snowDeformation` in terrain material
2. **Bind deformation uniforms** in terrain drawable or material
3. **Integrate SnowDeformationManager** into World or RenderingManager
4. **Complete footprint shader setup** with actual shader loading
5. **Implement ping-pong buffer swap** in stampFootprint()
6. **Test basic deformation** rendering
7. **Complete snow detection** with actual layer queries
8. **Add decay system** (optional polish)
9. **Add settings** configuration
10. **Performance optimization** and polish

## Known Issues / Limitations

1. **Snow detection is stubbed**: Currently returns `true` everywhere for testing
2. **Footprint stamping incomplete**: Shaders created but not loaded/bound
3. **No decay**: Footprints permanent until system restarted
4. **No settings**: All parameters hardcoded
5. **Shader define missing**: Terrain shader won't compile deformation code yet

## Estimated Time to Completion

- **Critical path** (get it working): 4-6 hours
  - Shader define: 30 min
  - Uniform binding: 1 hour
  - Manager integration: 1-2 hours
  - Footprint shader setup: 1-2 hours
  - Testing and debugging: 1 hour

- **Full featured** (polish): +4-6 hours
  - Snow detection: 2-3 hours
  - Decay system: 2-3 hours

- **Total MVP**: 8-12 hours

## Success Criteria

### Minimum Viable Product
- [x] Subdivision system working
- [x] Deformation textures created
- [x] Shaders written
- [ ] Terrain vertices displaced by deformation texture
- [ ] Footprints visible when walking
- [ ] System can be enabled/disabled
- [ ] No crashes or major bugs
- [ ] 60 FPS maintained

### Full Feature Set
- [ ] Texture-based snow detection working
- [ ] Decay system restores snow over time
- [ ] Settings configurable
- [ ] Memory usage optimized
- [ ] Works in all snow regions (Solstheim, modded areas)

---

**Document Version**: 1.0
**Last Updated**: 2025-11-15
**Status**: Core implementation complete, integration pending
