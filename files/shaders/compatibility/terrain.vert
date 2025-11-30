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

// ============================================================================
// TERRAIN DEFORMATION SYSTEM - Multi-Terrain Support (Snow, Ash, Mud)
// ============================================================================
// Uses RTT texture for deformation (Unified System)
// Terrain weights computed per-vertex for smooth transitions
// ============================================================================
uniform sampler2D snowDeformationMap;    // RTT texture containing footprints
uniform vec3 snowRTTWorldOrigin;           // Center of the RTT area in world space
uniform float snowRTTScale;                // Size of the RTT area in world units
uniform float snowCurrentTime;             // Current game time
uniform bool snowDeformationEnabled;       // Runtime enable/disable
uniform vec3 chunkWorldOffset;             // Chunk's world position (for local->world conversion)

// Terrain-specific deformation parameters
uniform float snowDeformationDepth;        // Snow deformation depth
uniform float ashDeformationDepth;         // Ash deformation depth (default 20)
uniform float mudDeformationDepth;         // Mud deformation depth (default 10)

// Terrain weight vertex attribute (snow, ash, mud, rock) - per vertex
attribute vec4 terrainWeights;             // x=snow, y=ash, z=mud, w=rock

varying float vDeformationFactor;          // Passed to fragment shader for POM/Visuals
varying vec3 passWorldPos;                 // World position for POM
varying float vMaxDepth;                   // Max deformation depth for this vertex

void main(void)
{
    vec4 vertex = gl_Vertex;

    // ========================================================================
    // TERRAIN DEFORMATION - Multi-Terrain Support (Snow, Ash, Mud)
    // ========================================================================
    // Loop through footprint positions, apply weighted deformation
    // Vertices have per-terrain weights for smooth transitions
    // ========================================================================
    vDeformationFactor = 0.0;

    if (snowDeformationEnabled)
    {
        // Convert vertex from chunk-local to world space
        vec3 worldPos = vertex.xyz + chunkWorldOffset;

        // Read terrain weights for this vertex
        vec4 weights = terrainWeights;

        // Calculate terrain-specific lift and max deformation based on weights
        float baseLift = weights.x * snowDeformationDepth +
                        weights.y * ashDeformationDepth +
                        weights.z * mudDeformationDepth;

        // Only process if this vertex is deformable (not pure rock)
        if (baseLift > 0.01)
        {
            // Calculate UVs for the deformation map
            // Map world position to [0, 1] texture space based on RTT origin and scale
            vec2 deformUV = (worldPos.xy - snowRTTWorldOrigin.xy) / snowRTTScale + 0.5;

            // Sample the deformation map (Red channel contains deformation factor 0..1)
            // Check bounds to avoid sampling outside the RTT area (though clamp to border handles this usually)
            if (deformUV.x >= 0.0 && deformUV.x <= 1.0 && deformUV.y >= 0.0 && deformUV.y <= 1.0)
            {
                vDeformationFactor = texture2D(snowDeformationMap, deformUV).r;
            }

            // Apply deformation: raise terrain by baseLift, then subtract where footprints are
            vertex.z += baseLift * (1.0 - vDeformationFactor);
            // vertex.z += 0.0;
            
            vMaxDepth = baseLift;
        }
        else
        {
            vMaxDepth = 0.0;
        }
        
        passWorldPos = worldPos; // Pass world position to fragment shader
    }
    else
    {
        passWorldPos = vertex.xyz + chunkWorldOffset;
        vMaxDepth = 0.0;
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
