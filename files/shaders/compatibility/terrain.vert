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
// SNOW DEFORMATION SYSTEM - Vertex Shader Array Approach
// ============================================================================
// Uses array of footprint positions passed from C++ instead of RTT texture
// Simple, efficient, no rendering complexity
// ============================================================================
uniform vec3 snowFootprintPositions[500];  // Array of footprint positions (X, Y, timestamp)
uniform int snowFootprintCount;            // Number of active footprints
uniform float snowFootprintRadius;         // Footprint radius in world units
uniform float snowDeformationDepth;        // Maximum deformation depth
uniform float snowCurrentTime;             // Current game time
uniform float snowDecayTime;               // Time for trails to fully fade (default 180s)
uniform bool snowDeformationEnabled;       // Runtime enable/disable
uniform vec3 chunkWorldOffset;             // Chunk's world position (for local->world conversion)

void main(void)
{
    vec4 vertex = gl_Vertex;

    // ========================================================================
    // SNOW DEFORMATION - Vertex Shader Array Approach
    // ========================================================================
    // Loop through footprint positions, apply deformation where close
    //
    // TODO: Add texture-weighted deformation
    // Currently all vertices deform equally. Future enhancement:
    // - Sample terrain textures at vertex position
    // - Weight deformation by texture type (snow=1.0, rock=0.0, mixed=0.5)
    // - This allows gradual transitions between terrain types
    // ========================================================================
    if (snowDeformationEnabled && snowFootprintCount > 0)
    {
        // Convert vertex from chunk-local to world space
        vec3 worldPos = vertex.xyz + chunkWorldOffset;

        // Accumulate total deformation from all nearby footprints
        float totalDeformation = 0.0;

        // TODO: Sample texture weight here for texture-based deformation
        // float terrainWeight = sampleTerrainTexture(worldPos.xy);
        // For now, assume uniform deformation (weight = 1.0)

        // Loop through all active footprints
        for (int i = 0; i < snowFootprintCount; i++)
        {
            vec3 footprint = snowFootprintPositions[i];
            vec2 footprintPos = footprint.xy;  // X, Y position
            float timestamp = footprint.z;     // Time when created

            // Calculate distance from vertex to footprint center (ground plane only)
            vec2 diff = worldPos.xy - footprintPos;
            float dist = length(diff);

            // Skip if outside footprint radius
            if (dist > snowFootprintRadius)
                continue;

            // Calculate age-based decay
            float age = snowCurrentTime - timestamp;
            float decayFactor = clamp(age / snowDecayTime, 0.0, 1.0);

            // Calculate distance-based falloff (smooth circular depression)
            float radiusFactor = 1.0 - (dist / snowFootprintRadius);
            radiusFactor = smoothstep(0.0, 1.0, radiusFactor);

            // Combine distance falloff with decay
            float deformation = snowDeformationDepth * radiusFactor * (1.0 - decayFactor);

            // Accumulate (take maximum, not sum, to avoid over-deepening)
            totalDeformation = max(totalDeformation, deformation);
        }

        // Apply deformation: raise terrain uniformly, then subtract where footprints are
        vertex.z += snowDeformationDepth - totalDeformation;

        // Result:
        // - Untouched snow: +snowDeformationDepth (raised)
        // - Fresh footprint: +0 (ground level)
        // - Old footprint: gradually returns to +snowDeformationDepth (restored)
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
