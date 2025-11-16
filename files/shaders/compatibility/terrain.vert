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
uniform vec3 chunkWorldOffset;            // Chunk's world position (for local->world conversion)

void main(void)
{
    vec4 vertex = gl_Vertex;

    // SNOW DEFORMATION DIAGNOSTIC TEST
    // CRITICAL: Test if shader is even running by doing unconditional deformation
    // FIXED: Use Z axis (up in OpenMW), not Y axis!

    

    // ============================================================================
    // PROGRESSIVE DIAGNOSTIC TESTS - Uncomment ONE test at a time, in order
    // Using GEOMETRY (altitude) tests - much more obvious than color!
    // ============================================================================

    // TEST 1: Is the shader even running?
    // Expected: ALL terrain rises by 500 units (very obvious!)
    // If this doesn't work: Shader isn't being used at all OR terrain uses multiple shaders
    /*
    vertex.z += 500.0;
    */

    // TEST 2: Is snowDeformationEnabled uniform being set to true?
    // Expected: ALL terrain rises by 500 units when system is active
    // If this doesn't work: The uniform isn't being set or is false
    /*
    if (snowDeformationEnabled)
    {
        vertex.z += 500.0;
    }
    */

    // TEST 3: Are uniforms being passed correctly?
    // Expected: Terrain rises by different amounts based on snowDeformationRadius
    // If rises by 500: radius is set correctly (150 * ~3.33 = 500)
    // If no rise: radius is zero or uniform not set
    /*
    if (snowDeformationEnabled)
    {
        vertex.z += snowDeformationRadius * 3.33;  // Should be 500 units if radius=150
    }
    */

    // TEST 4: Is chunkWorldOffset being set?
    // Expected: Terrain creates a "staircase" pattern based on chunk positions
    // If all flat: chunkWorldOffset is zero (PROBLEM!)
    // If staircase/waves: chunkWorldOffset is being set correctly

    /*
    if (snowDeformationEnabled)
    {
        // Create visible pattern from chunk offset
        // NOTE: chunkWorldOffset is Vec3(X, Y, 0) where X=East, Y=North, Z=Up (always 0 for terrain)
        // chunkWorldOffset is in WORLD coordinates (e.g., 4096, 8192, etc.)
        float pattern = mod(abs(chunkWorldOffset.x) + abs(chunkWorldOffset.y), 1000.0);
        vertex.z += pattern * 0.5;  // Creates steps/waves
    }
    */
    

    // TEST 5: Is world position calculation working?
    // Expected: Terrain within 300 units of player rises 500 units (circular plateau)
    // If no plateau: World position calculation is broken
    /*
    if (snowDeformationEnabled)
    {
        vec3 worldPos = vertex.xyz + chunkWorldOffset;
        vec2 relativePos = worldPos.xy - snowDeformationCenter;
        float distFromPlayer = length(relativePos);

        if (distFromPlayer < snowDeformationRadius * 2.0)  // 300 units
            vertex.z += 500.0;  // Circular plateau around player
    }
    */

    // TEST 6: Are UVs being calculated correctly?
    // Expected: Smooth circular cone rising 500 units at center, tapering to 0 at edges
    // Center under player = peak (500), edges (150 units away) = ground level
    /*
    if (snowDeformationEnabled)
    {
        vec3 worldPos = vertex.xyz + chunkWorldOffset;
        vec2 relativePos = worldPos.xy - snowDeformationCenter;
        vec2 deformUV = (relativePos / snowDeformationRadius) * 0.5 + 0.5;

        if (deformUV.x >= 0.0 && deformUV.x <= 1.0 && deformUV.y >= 0.0 && deformUV.y <= 1.0)
        {
            // Create cone shape from UVs (center high, edges low)
            float distFromCenter = length(deformUV - vec2(0.5, 0.5));
            float height = max(0.0, 1.0 - distFromCenter * 2.0);  // 1.0 at center, 0.0 at edge
            vertex.z += height * 500.0;  // 500 units at center
        }
    }
    */

    // TEST 7: Can we sample the texture at center?
    // Expected: ALL terrain rises/sinks based on texture center value
    // Should rise/sink by 100 units everywhere if test pattern exists (50 * 2)
    /*
    if (snowDeformationEnabled)
    {
        vec2 testUV = vec2(0.5, 0.5);  // Always sample center
        float deformationDepth = texture2D(snowDeformationMap, testUV).r;
        vertex.z -= deformationDepth * 2.0;  // Should be -100 if test pattern = 50
    }
    */

    // TEST 8: Can we sample the texture with calculated UVs?
    // Expected: Circular depression following player (100 units deep at center)
    /*
    if (snowDeformationEnabled)
    {
        vec3 worldPos = vertex.xyz + chunkWorldOffset;
        vec2 relativePos = worldPos.xy - snowDeformationCenter;
        vec2 deformUV = (relativePos / snowDeformationRadius) * 0.5 + 0.5;

        float deformationDepth = texture2D(snowDeformationMap, deformUV).r;
        vertex.z -= deformationDepth * 2.0;  // 100 units at center (50 * 2)
    }
    */

    // FINAL IMPLEMENTATION: Raise terrain everywhere, then subtract deformation
    // This prevents character floating - they walk on the raised surface and sink into depressions

    if (snowDeformationEnabled)
    {
        vec3 worldPos = vertex.xyz + chunkWorldOffset;
        vec2 relativePos = worldPos.xy - snowDeformationCenter;
        vec2 deformUV = (relativePos / snowDeformationRadius) * 0.5 + 0.5;

        float deformationDepth = texture2D(snowDeformationMap, deformUV).r;

        // Raise all terrain by max deformation depth (8 units default)
        // Then subtract actual deformation - creates depression where player walks
        // Character physics sees raised terrain, walks into depressions naturally
        const float maxDeformationDepth = 8.0;  // Should match mDeformationDepth in C++
        vertex.z += maxDeformationDepth - deformationDepth;

        // This means:
        // - Where no deformation: vertex.z += 8.0 (terrain raised)
        // - Where full deformation (8 units): vertex.z += 8.0 - 8.0 = 0 (original height)
        // - Character walks on raised surface, depressions are relative to raised level
    }
    

    // ============================================================================
    // INSTRUCTIONS:
    // 1. Uncomment TEST 1 first - ALL terrain should rise massively
    // 2. If it works, comment it out and try TEST 2
    // 3. Continue through tests until one FAILS
    // 4. The first test that fails tells us where the problem is!
    //
    // IMPORTANT: Tests use Z axis (up in OpenMW). Positive Z = up, negative Z = down
    // ============================================================================


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
