#version 120

// ============================================================================
// SNOW TRAIL SYSTEM - Footprint Stamping Shader
// ============================================================================
// This shader implements a non-additive trail system with time-based decay
//
// Features:
// - Non-additive deformation: Multiple passes don't deepen snow
// - Age preservation: Trampled snow doesn't refresh on repeat passes
// - Smooth circular falloff for realistic footprint shape
//
// Texture Channels:
// - R (Red):   Deformation depth (0.0 = no deformation, 1.0 = full depth)
// - G (Green): Age timestamp (game time when first deformed)
// - B (Blue):  Unused (reserved for future features)
// - A (Alpha): Always 1.0
// ============================================================================

uniform sampler2D previousDeformation;  // Previous frame's deformation texture
uniform vec2 footprintCenter;           // World XZ position of footprint center
uniform float footprintRadius;          // Footprint radius in world units
uniform float deformationDepth;         // Maximum deformation depth in world units
uniform float currentTime;              // Current game time for age tracking

varying vec2 worldPos;   // World XZ position for this fragment
varying vec2 texUV;      // Texture UV for sampling previous deformation

void main()
{
    // Sample previous deformation state from ping-pong buffer
    vec4 prevDeform = texture2D(previousDeformation, texUV);
    float prevDepth = prevDeform.r;  // Red channel = deformation depth
    float prevAge = prevDeform.g;    // Green channel = timestamp when first deformed

    // Calculate distance from current footprint center in world space
    float dist = length(worldPos - footprintCenter);

    // Smooth circular falloff for realistic footprint shape
    // - Full influence at center (0.0)
    // - Smooth transition from 50% to 100% of radius
    // - Zero influence beyond radius
    float influence = 1.0 - smoothstep(footprintRadius * 0.5, footprintRadius, dist);

    // Calculate new deformation depth from this footprint
    float newFootprintDepth = influence * deformationDepth;

    // ========================================================================
    // NON-ADDITIVE TRAIL LOGIC
    // ========================================================================
    // Key behavior: Use max() blending so multiple passes don't deepen snow
    // This creates a "plowing through snow" effect where trails stay constant
    // ========================================================================
    float newDepth = max(prevDepth, newFootprintDepth);

    // ========================================================================
    // AGE PRESERVATION LOGIC
    // ========================================================================
    // CRITICAL: Do NOT reset age on repeat passes!
    //
    // If there's already deformation (prevDepth > 0.01):
    //   - Keep the original age (prevAge)
    //   - This ensures trails decay based on when they were FIRST created
    //   - Walking on the same trail again doesn't refresh the timer
    //
    // If this is fresh snow (prevDepth <= 0.01):
    //   - Set age to currentTime (mark as newly deformed)
    //
    // This implements the "plowing through snow" effect:
    // - First pass creates trail with timestamp
    // - Subsequent passes don't refresh the trail or its decay timer
    // ========================================================================
    float age;
    if (prevDepth > 0.01)
    {
        // Already deformed - preserve original age (no refresh)
        age = prevAge;
    }
    else if (newFootprintDepth > 0.01)
    {
        // Fresh snow being deformed - mark with current time
        age = currentTime;
    }
    else
    {
        // No deformation at all - keep previous age (if any)
        age = prevAge;
    }

    // Output deformation data
    // R = depth (0.0 to 1.0, scaled by deformationDepth in terrain shader)
    // G = age (game time when first deformed, used for decay calculation)
    // B = unused (reserved)
    // A = 1.0 (always opaque)
    gl_FragColor = vec4(newDepth, age, 0.0, 1.0);
}
