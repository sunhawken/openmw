#version 120

void main()
{
    // Apply projection matrix to transform from [0,1] quad space to clip space [-1,1]
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
    gl_TexCoord[0] = gl_MultiTexCoord0;
}
