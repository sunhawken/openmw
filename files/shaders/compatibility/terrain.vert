#version 120

#if @useUBO
    #extension GL_ARB_uniform_buffer_object : require
#endif

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#include "lib/core/vertex.h.glsl"
varying vec2 uv;
varying float euclideanDepth;
varying float linearDepth;

#define PER_PIXEL_LIGHTING (@normalMap || @specularMap || @forcePPL)

#if !PER_PIXEL_LIGHTING
centroid varying vec3 passLighting;
centroid varying vec3 passSpecular;
centroid varying vec3 shadowDiffuseLighting;
centroid varying vec3 shadowSpecularLighting;
#endif
varying vec3 passViewPos;
varying vec3 passNormal;

#include "vertexcolors.glsl"
#include "shadows_vertex.glsl"
#include "compatibility/normals.glsl"

#include "lib/light/lighting.glsl"
#include "lib/view/depth.glsl"

// Snow deformation system - ALWAYS ENABLED FOR TESTING
uniform sampler2D snowDeformationMap;     // Deformation texture (R=depth, G=age)
uniform vec2 snowDeformationCenter;       // World XZ center of deformation texture
uniform float snowDeformationRadius;      // World radius covered by texture
uniform bool snowDeformationEnabled;      // Runtime enable/disable

void main(void)
{
    vec4 vertex = gl_Vertex;

    // SNOW DEFORMATION DIAGNOSTIC TEST
    // CRITICAL: Test if shader is even running by doing unconditional deformation

    // TEST 1: UNCOMMENT THIS to verify shader runs (entire terrain drops 100 units)
    vertex.y -= 100.0;  // THIS SHOULD BE VISIBLE - terrain drops ~1.4 meters

    // TEST 2: If TEST 1 works, comment it out and uncomment this to test the uniform
    /*
    if (snowDeformationEnabled)
    {
        // If you see terrain drop here, the uniform is being set
        vertex.y -= 100.0;
    }
    */

    // TEST 3: If TEST 2 works, comment it and uncomment this to test texture sampling
    /*
    if (snowDeformationEnabled)
    {
        vec2 testUV = vec2(0.5, 0.5);
        float deformationDepth = texture2D(snowDeformationMap, testUV).r;

        // Debug: multiply by large value to see if we're reading ANYTHING
        vertex.y -= deformationDepth * 2.0;  // Should be ~100 units at center
    }
    */

    // TEST 4: If TEST 3 works, use calculated UV to follow player
    /*
    if (snowDeformationEnabled)
    {
        vec3 worldPos = vertex.xyz;
        vec2 relativePos = worldPos.xz - snowDeformationCenter;
        vec2 deformUV = (relativePos / snowDeformationRadius) * 0.5 + 0.5;
        float deformationDepth = texture2D(snowDeformationMap, deformUV).r;
        vertex.y -= deformationDepth;
    }
    */

    gl_Position = modelToClip(vertex);

    vec4 viewPos = modelToView(vertex);
    gl_ClipVertex = viewPos;
    euclideanDepth = length(viewPos.xyz);
    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);

    passColor = gl_Color;
    passNormal = gl_Normal.xyz;
    passViewPos = viewPos.xyz;
    normalToViewMatrix = gl_NormalMatrix;

#if @normalMap
    mat3 tbnMatrix = generateTangentSpace(vec4(1.0, 0.0, 0.0, -1.0), passNormal);
    tbnMatrix[0] = -normalize(cross(tbnMatrix[2], tbnMatrix[1])); // our original tangent was not at a 90 degree angle to the normal, so we need to rederive it
    normalToViewMatrix *= tbnMatrix;
#endif

#if !PER_PIXEL_LIGHTING || @shadows_enabled
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#endif

#if !PER_PIXEL_LIGHTING
    vec3 diffuseLight, ambientLight, specularLight;
    doLighting(viewPos.xyz, viewNormal, gl_FrontMaterial.shininess, diffuseLight, ambientLight, specularLight, shadowDiffuseLighting, shadowSpecularLighting);
    passLighting = getDiffuseColor().xyz * diffuseLight + getAmbientColor().xyz * ambientLight + getEmissionColor().xyz;
    passSpecular = getSpecularColor().xyz * specularLight;
    clampLightingResult(passLighting);
    shadowDiffuseLighting *= getDiffuseColor().xyz;
    shadowSpecularLighting *= getSpecularColor().xyz;
#endif

    uv = gl_MultiTexCoord0.xy;

#if (@shadows_enabled)
    setupShadowCoords(viewPos, viewNormal);
#endif
}
