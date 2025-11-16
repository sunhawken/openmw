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
// Uses array of footprint positions passed from C++ instead of RTT texture
// Terrain weights computed per-vertex for smooth transitions
// ============================================================================
uniform vec3 snowFootprintPositions[500];  // Array of footprint positions (X, Y, timestamp)
uniform int snowFootprintCount;            // Number of active footprints
uniform float snowFootprintRadius;         // Footprint radius in world units
uniform float snowDeformationDepth;        // Maximum deformation depth
uniform float snowCurrentTime;             // Current game time
uniform float snowDecayTime;               // Time for trails to fully fade (default 180s)
uniform bool snowDeformationEnabled;       // Runtime enable/disable
uniform vec3 chunkWorldOffset;             // Chunk's world position (for local->world conversion)

// Terrain-specific deformation parameters
uniform float ashDeformationDepth;         // Ash deformation depth (default 20)
uniform float mudDeformationDepth;         // Mud deformation depth (default 10)

// Terrain weight vertex attribute (snow, ash, mud, rock) - per vertex
attribute vec4 terrainWeights;             // x=snow, y=ash, z=mud, w=rock

void main(void)
{
    vec4 vertex = gl_Vertex;

    // ========================================================================
    // TERRAIN DEFORMATION - Multi-Terrain Support (Snow, Ash, Mud)
    // ========================================================================
    // Loop through footprint positions, apply weighted deformation
    // Vertices have per-terrain weights for smooth transitions
    // ========================================================================
    if (snowDeformationEnabled && snowFootprintCount > 0)
    {
        // Convert vertex from chunk-local to world space
        vec3 worldPos = vertex.xyz + chunkWorldOffset;

        // Read terrain weights for this vertex (default to rock if not provided)
        vec4 weights = vec4(0.0, 0.0, 0.0, 1.0);  // Default: pure rock (no deformation)
        #ifdef GL_ARB_vertex_shader
            weights = terrainWeights;
        #endif

        // Calculate terrain-specific lift and max deformation based on weights
        // Each terrain type has different deformation characteristics:
        // - Snow: deep, soft (100 units)
        // - Ash: medium (20 units)
        // - Mud: shallow (10 units)
        // - Rock: no deformation (0 units)
        float baseLift = weights.x * snowDeformationDepth +
                        weights.y * ashDeformationDepth +
                        weights.z * mudDeformationDepth;

        float maxDeform = baseLift;  // Maximum deformation matches base lift

        // Only process if this vertex is deformable (not pure rock)
        if (maxDeform > 0.01)
        {
            // Accumulate total deformation from all nearby footprints
            float totalDeformation = 0.0;

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
                // Use maxDeform (weighted) instead of uniform snowDeformationDepth
                float deformation = maxDeform * radiusFactor * (1.0 - decayFactor);

                // Accumulate (take maximum, not sum, to avoid over-deepening)
                totalDeformation = max(totalDeformation, deformation);
            }

            // Apply deformation: raise terrain by baseLift, then subtract where footprints are
            vertex.z += baseLift - totalDeformation;

            // Result:
            // - Pure snow vertex: lifts 100, deforms up to 100
            // - Pure ash vertex: lifts 20, deforms up to 20
            // - Pure mud vertex: lifts 10, deforms up to 10
            // - Mixed (50% snow, 50% rock): lifts 50, deforms up to 50
            // - Pure rock vertex: lifts 0, no deformation (excluded by if check)
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
