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

    // SNOW DEFORMATION - ALWAYS ACTIVE (NO @defines)
    if (snowDeformationEnabled)
    {
        // Calculate world position of vertex (assuming model matrix is identity for terrain)
        vec3 worldPos = vertex.xyz;

        // Convert world XZ to deformation texture UV
        vec2 relativePos = worldPos.xz - snowDeformationCenter;
        vec2 deformUV = (relativePos / snowDeformationRadius) * 0.5 + 0.5;

        // Check if vertex is within deformation texture bounds
        if (deformUV.x >= 0.0 && deformUV.x <= 1.0 && deformUV.y >= 0.0 && deformUV.y <= 1.0)
        {
            // Sample deformation depth (red channel)
            float deformationDepth = texture2D(snowDeformationMap, deformUV).r;

            // DEBUG: Always displace by 100 units to test if this code runs
            vertex.y -= 100.0; // TESTING - should see terrain drop everywhere

            // Displace vertex downward (negative Y)
            // vertex.y -= deformationDepth;
        }
    }

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
