#version 120

// Fragment shader for stamping footprints into deformation texture
// Uses ping-pong rendering to accumulate footprints over time

uniform sampler2D previousDeformation;  // Previous frame's deformation
uniform vec2 footprintCenter;           // World XZ position of footprint center
uniform float footprintRadius;          // Footprint radius in world units
uniform float deformationDepth;         // Maximum deformation depth
uniform float currentTime;              // Game time for aging

varying vec2 worldPos;   // World XZ position for this fragment
varying vec2 texUV;      // Texture UV for sampling previous deformation

void main()
{
    // Sample previous deformation state
    vec4 prevDeform = texture2D(previousDeformation, texUV);
    float prevDepth = prevDeform.r;  // Red channel = deformation depth
    float prevAge = prevDeform.g;    // Green channel = timestamp when deformed

    // Calculate distance from footprint center in world space
    float dist = length(worldPos - footprintCenter);

    // Smooth circular falloff for footprint
    // Creates soft-edged circular depression
    float influence = 1.0 - smoothstep(footprintRadius * 0.5, footprintRadius, dist);

    // New deformation depth (additive, keep maximum)
    // This allows footprints to overlap and deepen snow
    float newDepth = max(prevDepth, influence * deformationDepth);

    // Update timestamp where new footprint is applied
    // This is used for decay - newer footprints decay slower
    float age = (influence > 0.01) ? currentTime : prevAge;

    // Output: R = depth, G = age, B = unused, A = 1.0
    gl_FragColor = vec4(newDepth, age, 0.0, 1.0);
}
