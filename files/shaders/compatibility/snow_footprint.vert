#version 120

// Vertex shader for stamping footprints into deformation texture
// Renders a full-screen quad in deformation texture space

uniform vec2 deformationCenter;  // Player XZ position in world space
uniform float deformationRadius; // Coverage radius in world units

varying vec2 worldPos;  // World XZ position for this fragment
varying vec2 texUV;     // Texture UV coordinates

void main()
{
    // Pass through vertex position
    gl_Position = gl_Vertex;

    // Calculate world position from NDC
    // NDC ranges from -1 to 1, map to world space around deformation center
    worldPos = deformationCenter + gl_Vertex.xy * deformationRadius;

    // UV coordinates for sampling previous deformation texture
    texUV = gl_MultiTexCoord0.xy;
}
