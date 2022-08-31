#version 120

#include "openmw_vertex.h.glsl"

uniform vec3 color;
uniform vec3 trans;
uniform vec3 scale;
uniform int  useNormalAsColor;
uniform int  useAdvancedShader = 0;

centroid varying vec4 passColor;
varying vec3 vertexNormal;

void main()
{
    gl_Position = mw_modelToClip( vec4(gl_Vertex.xyz * scale + trans,1));
    if(useAdvancedShader == 0)
    {
        vertexNormal = vec3(1., 1., 1.);
        passColor  = gl_Color;
    }
    else
    {
        vertexNormal = useNormalAsColor == 1 ? vec3(1., 1., 1.) : gl_Normal.xyz;
        vec3 colorOut = useNormalAsColor == 1 ? gl_Normal.xyz : color;
        passColor = vec4(colorOut, 1.);
    }
    
}
