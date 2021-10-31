#version @GLSLVersion

#include "multiview_vertex.glsl"

centroid varying vec4 passColor;

void main()
{
    gl_Position = mw_stereoAwareProjectionMatrix() * (mw_stereoAwareModelViewMatrix() * gl_Vertex);

    passColor = gl_Color;
}
