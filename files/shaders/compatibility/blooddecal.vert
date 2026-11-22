#version 120
#include "lib/core/vertex.h.glsl"
varying vec2 bloodUV;
varying vec3 bloodViewPos;
void main()
{
    gl_Position = modelToClip(gl_Vertex);
    vec4 viewPos = modelToView(gl_Vertex);
    gl_ClipVertex = viewPos;
    bloodViewPos = viewPos.xyz;
    bloodUV = gl_MultiTexCoord0.xy;
}
