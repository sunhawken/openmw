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

    // DEBUG: Visualize Snow RTT
    vec2 debugUV = (passWorldPos.xy - snowRTTWorldOrigin.xy) / snowRTTScale + 0.5;
    if (debugUV.x >= 0.0 && debugUV.x <= 1.0 && debugUV.y >= 0.0 && debugUV.y <= 1.0)
    {
        float rttVal = texture2D(snowDeformationMap, debugUV).r;
        if (rttVal > 0.0)
        {
            gl_FragData[0].rgb = vec3(1.0, 0.0, 0.0) * rttVal; // Red footprints
        }
    }

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
        // We assume the "undeformed" snow surface is at flatZ
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

    // Apply visual darkening for deformed areas (wet/compressed snow/mud)
    // vDeformationFactor is 0..1 (1 = max deformation)
    diffuseColor.rgb *= (1.0 - deformationFactor * 0.4);

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
    vec3 viewNormal = normalToView(normal);
#else
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#endif

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
