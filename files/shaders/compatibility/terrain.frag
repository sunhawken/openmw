#version 120

#if @useUBO
    #extension GL_ARB_uniform_buffer_object : require
#endif

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

varying vec2 uv;
varying float vDeformationFactor; // From vertex shader
varying vec3 passWorldPos;          // From vertex shader
varying float vMaxDepth;            // From vertex shader

uniform sampler2D snowDeformationMap;
uniform vec3 snowRTTWorldOrigin;
uniform float snowRTTScale;

uniform sampler2D diffuseMap;

#if @normalMap
uniform sampler2D normalMap;
#endif

#if @blendMap
uniform sampler2D blendMap;
#endif

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

uniform vec2 screenRes;
uniform float far;

#include "vertexcolors.glsl"
#include "shadows_fragment.glsl"
#include "lib/light/lighting.glsl"
#include "lib/material/parallax.glsl"
#include "fog.glsl"
#include "compatibility/normals.glsl"

void main()
{
    vec2 adjustedUV = (gl_TextureMatrix[0] * vec4(uv, 0.0, 1.0)).xy;

#if @parallax
    float height = texture2D(normalMap, adjustedUV).a;
    adjustedUV += getParallaxOffset(transpose(normalToViewMatrix) * normalize(-passViewPos), height);
#endif
    vec4 diffuseTex = texture2D(diffuseMap, adjustedUV);
    gl_FragData[0] = vec4(diffuseTex.xyz, 1.0);

    vec4 diffuseColor = getDiffuseColor();
    
    // ========================================================================
    // SNOW POM & DARKENING
    // ========================================================================
    
    float deformationFactor = vDeformationFactor;
    
    if (vMaxDepth > 0.0)
    {
        // 1. Calculate View Vector in World Space
        // passViewPos is vector from Camera to Vertex in View Space
        // We need it in World Space. Assuming standard OpenMW View matrix (Rotation only for normals)
        // viewDirWorld = inverse(View) * viewDirView
        // We use gl_ModelViewMatrixInverse because for terrain chunks Model is just translation
        vec3 viewDir = normalize((gl_ModelViewMatrixInverse * vec4(passViewPos, 0.0)).xyz);
        
        // 2. Raymarch Setup
        // Calculate the undeformed snow surface height
        // passWorldPos.z is already deformed, so we add back the deformation to get the original flat surface
        float flatZ = passWorldPos.z + vDeformationFactor * vMaxDepth;
        
        vec3 p = passWorldPos;
        vec3 v = viewDir;
        
        // Raymarch parameters
        const int STEPS = 10;
        float stepSize = vMaxDepth * 0.15; // Step size relative to max depth
        
        // Refine starting point: back up a bit to ensure we don't start inside
        p -= v * stepSize * 2.0;
        
        for (int i = 0; i < STEPS; ++i)
        {
            p += v * stepSize;
            
            // Calculate RTT UV
            vec2 rttUV = (p.xy - snowRTTWorldOrigin.xy) / snowRTTScale + 0.5;
            
            // Check bounds
            if (rttUV.x >= 0.0 && rttUV.x <= 1.0 && rttUV.y >= 0.0 && rttUV.y <= 1.0)
            {
                float r = texture2D(snowDeformationMap, rttUV).r;
                
                // Current depth of the hole at this point
                // Surface is at flatZ. Hole bottom is at flatZ - vMaxDepth.
                // Actual surface height = flatZ - r * vMaxDepth.
                float surfaceH = flatZ - r * vMaxDepth;
                
                // If ray is below surface, we hit
                if (p.z < surfaceH)
                {
                    deformationFactor = r;
                    
                    // TODO: Calculate UV offset for texture shifting
                    // For now, we just use the updated deformation factor for correct darkening
                    break;
                }
            }
        }
    }

    // Apply visual effects for deformed areas (wet/compressed snow/mud)
    // deformationFactor can be:
    //   positive (0 to 1) = depression (footprint center) - darken more
    //   negative (-0.5 to 0) = rim elevation - slight brightening
    //   zero = flat snow

    float darkeningFactor;
    if (deformationFactor > 0.0)
    {
        // Depression: darken progressively (compressed/wet snow appearance)
        // Stronger darkening for deeper impressions using smoothstep
        darkeningFactor = smoothstep(0.0, 1.0, deformationFactor) * 0.5;
    }
    else
    {
        // Rim: very slight brightening (snow pushed up catches more light)
        darkeningFactor = deformationFactor * 0.15; // Negative = brightening
    }

    diffuseColor.rgb *= (1.0 - darkeningFactor);

    gl_FragData[0].a *= diffuseColor.a;

#if @blendMap
    vec2 blendMapUV = (gl_TextureMatrix[1] * vec4(uv, 0.0, 1.0)).xy;
    gl_FragData[0].a *= texture2D(blendMap, blendMapUV).a;
#endif

#if @normalMap
    vec4 normalTex = texture2D(normalMap, adjustedUV);
    vec3 normal = normalTex.xyz * 2.0 - 1.0;
#if @reconstructNormalZ
    normal.z = sqrt(1.0 - dot(normal.xy, normal.xy));
#endif
    // If snow deformation is active, perturb the normal map normal
    if (vMaxDepth > 0.0)
    {
        vec2 deformUV = (passWorldPos.xy - snowRTTWorldOrigin.xy) / snowRTTScale + 0.5;
        if (deformUV.x >= 0.0 && deformUV.x <= 1.0 && deformUV.y >= 0.0 && deformUV.y <= 1.0)
        {
            float texelSize = 1.0 / 2048.0;
            float h = texture2D(snowDeformationMap, deformUV).r;
            float h_r = texture2D(snowDeformationMap, deformUV + vec2(texelSize, 0.0)).r;
            float h_u = texture2D(snowDeformationMap, deformUV + vec2(0.0, texelSize)).r;
            
            // Calculate derivatives
            // Z = Base - h * MaxDepth
            // dZ/dX = (Z_r - Z) / step = ((-h_r) - (-h)) * MaxDepth / step = (h - h_r) * MaxDepth / step
            float stepWorld = snowRTTScale * texelSize;
            float dX = (h - h_r) * vMaxDepth / stepWorld;
            float dY = (h - h_u) * vMaxDepth / stepWorld;
            
            // Perturb the tangent-space normal? 
            // No, 'normal' here is Tangent Space (from normal map).
            // We calculated dX, dY in World Space (assuming Terrain UV aligns with World XY).
            // We need to convert World Space perturbation to Tangent Space?
            // Or just perturb the View Space normal later?
            // Perturbing View Space is easier if we don't have TBN here.
            // But we DO have TBN implicit in 'normalToView' if normalMap is on?
            // Actually, 'normalToView' usually multiplies by TBN.
            // Let's perturb the RESULT of normalToView.
        }
    }
    vec3 viewNormal = normalToView(normal);
#else
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#endif

    // Apply Snow Deformation Normal Perturbation (Post-NormalMap / Post-VertexNormal)
    // This creates the lighting response that makes footprints look 3D
    if (vMaxDepth > 0.0)
    {
        vec2 deformUV = (passWorldPos.xy - snowRTTWorldOrigin.xy) / snowRTTScale + 0.5;
        if (deformUV.x >= 0.0 && deformUV.x <= 1.0 && deformUV.y >= 0.0 && deformUV.y <= 1.0)
        {
            // Use larger sample offset for smoother normals (matches blur spread)
            float texelSize = 2.0 / 2048.0; // 2x texel for smoother gradients

            // Central difference for more accurate normals
            float h_l = texture2D(snowDeformationMap, deformUV + vec2(-texelSize, 0.0)).r;
            float h_r = texture2D(snowDeformationMap, deformUV + vec2(texelSize, 0.0)).r;
            float h_d = texture2D(snowDeformationMap, deformUV + vec2(0.0, -texelSize)).r;
            float h_u = texture2D(snowDeformationMap, deformUV + vec2(0.0, texelSize)).r;

            // Calculate gradients using central difference
            float stepWorld = snowRTTScale * texelSize * 2.0; // Full span = 2 * texelSize
            float dX = (h_l - h_r) * vMaxDepth / stepWorld;
            float dY = (h_d - h_u) * vMaxDepth / stepWorld;

            // Amplify normal strength for more dramatic lighting
            const float normalStrength = 1.5;
            dX *= normalStrength;
            dY *= normalStrength;

            // Construct perturbation vector in World Space (Z-up)
            // Normal = cross(tangentX, tangentY) = (-dX, -dY, 1)
            vec3 worldPerturb = normalize(vec3(-dX, -dY, 1.0));

            // Convert to View Space
            vec3 viewPerturb = normalize(gl_NormalMatrix * worldPerturb);

            // Get the world-up direction in view space for blending
            vec3 viewUp = normalize(gl_NormalMatrix * vec3(0.0, 0.0, 1.0));

            // Calculate the perturbation as deviation from world-up
            vec3 diff = viewPerturb - viewUp;

            // Apply perturbation to existing normal
            // This correctly combines terrain slope with footprint shape
            viewNormal = normalize(viewNormal + diff);
        }
    }

    float shadowing = unshadowedLightRatio(linearDepth);
    vec3 lighting, specular;
#if !PER_PIXEL_LIGHTING
    lighting = passLighting + shadowDiffuseLighting * shadowing;
    specular = passSpecular + shadowSpecularLighting * shadowing;
#else
#if @specularMap
    float shininess = 128.0; // TODO: make configurable
    vec3 specularColor = vec3(diffuseTex.a);
#else
    float shininess = gl_FrontMaterial.shininess;
    vec3 specularColor = getSpecularColor().xyz;
#endif
    vec3 diffuseLight, ambientLight, specularLight;
    doLighting(passViewPos, viewNormal, shininess, shadowing, diffuseLight, ambientLight, specularLight);
    lighting = diffuseColor.xyz * diffuseLight + getAmbientColor().xyz * ambientLight + getEmissionColor().xyz;
    specular = specularColor * specularLight;
#endif

    clampLightingResult(lighting);
    gl_FragData[0].xyz = gl_FragData[0].xyz * lighting + specular;

    gl_FragData[0] = applyFogAtDist(gl_FragData[0], euclideanDepth, linearDepth, far);

#if !@disableNormals && @writeNormals
    gl_FragData[1].xyz = viewNormal * 0.5 + 0.5;
#endif

    applyShadowDebugOverlay();
}
