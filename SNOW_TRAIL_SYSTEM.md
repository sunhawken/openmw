# Snow Trail System Documentation

## Overview

This document describes the improved snow trail system for OpenMW, which implements realistic, non-additive snow deformation with time-based decay.

## Features

### ✅ Non-Additive Trails
- **Behavior**: Multiple passes over the same area don't deepen the snow
- **Implementation**: Uses `max()` blending instead of additive blending
- **Effect**: Creates a "plowing through snow" effect where trails stay constant depth

### ✅ Age Preservation (No Refresh)
- **Behavior**: Walking on existing trails doesn't reset the decay timer
- **Implementation**: Age timestamp is preserved on repeat passes
- **Effect**: Old trails continue to decay naturally, even if walked on again

### ✅ Time-Based Decay
- **Duration**: Trails fade out over 3 minutes (180 seconds) by default
- **Type**: Linear decay from full depth to zero
- **Configurability**: Decay time is adjustable via `mDecayTime` parameter

### ✅ Smooth Restoration
- **Update Rate**: Decay applied every 0.1 seconds
- **Visual**: Gradual, smooth restoration to pristine snow
- **Performance**: Minimal overhead, runs only when no other operations active

## Architecture

### Texture Channels

The deformation texture uses RGBA16F format with the following channel mapping:

| Channel | Purpose | Range | Description |
|---------|---------|-------|-------------|
| **R (Red)** | Deformation Depth | 0.0 - 1.0 | 0.0 = no deformation, 1.0 = full depth |
| **G (Green)** | Age Timestamp | Game time | When the area was first deformed |
| **B (Blue)** | Unused | - | Reserved for future features |
| **A (Alpha)** | Opacity | 1.0 | Always opaque |

### Coordinate System

OpenMW uses a **Z-up** coordinate system:
- **X**: East/West
- **Y**: North/South
- **Z**: Up/Down (altitude)

The trail texture follows the player on the **XY ground plane**, with Z representing altitude.

## Implementation Details

### Footprint Stamping Shader

**File**: `files/shaders/compatibility/snow_footprint.frag`

**Key Logic**:
```glsl
// Non-additive depth calculation
float newDepth = max(prevDepth, newFootprintDepth);

// Age preservation logic
if (prevDepth > 0.01) {
    // Already deformed - preserve original age (no refresh)
    age = prevAge;
} else if (newFootprintDepth > 0.01) {
    // Fresh snow - mark with current time
    age = currentTime;
} else {
    // No deformation - keep previous age
    age = prevAge;
}
```

### Decay Shader

**File**: Inline in `components/terrain/snowdeformation.cpp` (lines 709-759)

**Key Logic**:
```glsl
// Calculate time since first deformation
float timeSinceCreation = currentTime - age;

// Linear decay factor (0.0 = fresh, 1.0 = fully decayed)
float decayFactor = clamp(timeSinceCreation / decayTime, 0.0, 1.0);

// Reduce depth gradually
depth *= (1.0 - decayFactor);
```

### RTT System

**Components**:
- **Ping-pong buffers**: Two 1024x1024 RGBA16F textures
- **RTT camera**: Orthographic, top-down view
- **Three render groups**:
  1. **Footprint group**: Stamps new footprints
  2. **Blit group**: Scrolls texture when player moves
  3. **Decay group**: Applies time-based restoration

**Update Priority** (only ONE operation per frame):
1. **Blit** (highest) - Preserves trails when recentering
2. **Footprint** (medium) - Creates new deformation
3. **Decay** (lowest) - Gradual restoration

## Configuration

### Default Parameters

| Parameter | Default Value | Description |
|-----------|--------------|-------------|
| `DEFAULT_TRAIL_DECAY_TIME` | 180.0 seconds | Time for trails to completely fade |
| `mDecayUpdateInterval` | 0.1 seconds | How often decay is applied |
| `mTextureResolution` | 1024 | Deformation texture size |
| `mWorldTextureRadius` | 300.0 | Coverage radius in world units |
| `mFootprintRadius` | 60.0 | Footprint size (snow default) |
| `mFootprintInterval` | 2.0 | Distance between footprints |
| `mDeformationDepth` | 100.0 | Maximum deformation depth |

### Adjusting Decay Time

To change trail lifetime, modify `DEFAULT_TRAIL_DECAY_TIME` in `components/terrain/snowdeformation.cpp`:

```cpp
// For 5 minute trails
static constexpr float DEFAULT_TRAIL_DECAY_TIME = 300.0f;  // 5 minutes

// For 1 minute trails
static constexpr float DEFAULT_TRAIL_DECAY_TIME = 60.0f;   // 1 minute
```

## Modified Files

### Core Implementation
- `components/terrain/snowdeformation.hpp` - Manager class with trail system documentation
- `components/terrain/snowdeformation.cpp` - Trail creation, decay, and configuration

### Shaders
- `files/shaders/compatibility/snow_footprint.frag` - Non-additive footprint stamping with age preservation

## Testing

### Expected Behavior

1. **First Pass**:
   - Snow deforms to configured depth
   - Age timestamp is set to current game time
   - Trail appears at full visibility

2. **Repeat Pass (Same Location)**:
   - Depth remains unchanged (doesn't deepen)
   - Age timestamp preserved (doesn't reset)
   - Trail continues decaying from original timestamp

3. **Over Time**:
   - Trail gradually fades over 3 minutes
   - Linear decay from full depth to zero
   - After 3 minutes, snow fully restored

4. **Movement**:
   - Continuous trail behind player
   - Texture follows player smoothly
   - Blit preserves existing trails when recentering

## Performance

- **Memory**: 16 MB for ping-pong buffers (2x 1024x1024 RGBA16F)
- **CPU**: Minimal overhead for decay updates (every 0.1s)
- **GPU**: One RTT operation per frame (blit OR footprint OR decay)
- **Target**: <2ms frame time impact at 60 FPS

## Future Enhancements

Potential improvements for the trail system:

1. **Variable Decay Rate**: Faster decay in warmer regions, slower in cold
2. **Wind Effects**: Drift snow fills in trails faster downwind
3. **Depth-Based Decay**: Deeper snow takes longer to fill in
4. **Footprint Shapes**: Different patterns for different creatures
5. **Compression**: Store trails more efficiently for larger areas

## Known Limitations

1. **Texture Resolution**: Limited to 1024x1024, may show pixelation at close range
2. **Coverage Area**: 600x600 world units (300 radius), trails outside this area are lost
3. **Persistence**: Trails only persist while within texture coverage radius
4. **Single Player**: System tracks one player's trails (could extend to NPCs)

## Changelog

### Version 2.0 (Current)
- ✅ Implemented true non-additive trails
- ✅ Added age preservation (no refresh on repeat passes)
- ✅ Extended decay time to 3 minutes (was 2 minutes)
- ✅ Improved documentation and code clarity
- ✅ Added configuration constants

### Version 1.0 (Previous)
- Basic trail system with decay
- Additive behavior with age reset on repeat passes
- 2 minute decay time
