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
    // DEBUG: Visualize Snow RTT - REMOVED
    // vec2 debugUV = (passWorldPos.xy - snowRTTWorldOrigin.xy) / snowRTTScale + 0.5;
    // if (debugUV.x >= 0.0 && debugUV.x <= 1.0 && debugUV.y >= 0.0 && debugUV.y <= 1.0)
    // {
    //     ...
    // }

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
    if (vMaxDepth > 0.0)
    {
        vec2 deformUV = (passWorldPos.xy - snowRTTWorldOrigin.xy) / snowRTTScale + 0.5;
        if (deformUV.x >= 0.0 && deformUV.x <= 1.0 && deformUV.y >= 0.0 && deformUV.y <= 1.0)
        {
            float texelSize = 1.0 / 2048.0;
            float h = texture2D(snowDeformationMap, deformUV).r;
            float h_r = texture2D(snowDeformationMap, deformUV + vec2(texelSize, 0.0)).r;
            float h_u = texture2D(snowDeformationMap, deformUV + vec2(0.0, texelSize)).r;
            
            float stepWorld = snowRTTScale * texelSize;
            float dX = (h - h_r) * vMaxDepth / stepWorld;
            float dY = (h - h_u) * vMaxDepth / stepWorld;
            
            // Construct perturbation vector in World Space (assuming Z-up)
            // The slope vector is (1, 0, dX) and (0, 1, dY).
            // The normal is cross(slopeX, slopeY) = (-dX, -dY, 1).
            vec3 worldPerturb = normalize(vec3(-dX, -dY, 1.0));
            
            // We need to blend this with the existing viewNormal.
            // viewNormal is in View Space.
            // worldPerturb is in World Space.
            // Convert worldPerturb to View Space.
            // gl_NormalMatrix transforms Model->View. For terrain Model=World (rotation-wise).
            vec3 viewPerturb = normalize(gl_NormalMatrix * worldPerturb);
            
            // Blend normals. 
            // If we are in a hole (h > 0), we want the hole's normal.
            // If we are flat (h = 0), we want the original normal.
            // But the perturbation *is* the shape.
            // If h=0 everywhere, dX=0, dY=0, worldPerturb=(0,0,1).
            // If original normal was (0,0,1), it matches.
            // If original normal was tilted (hill), we want to combine them.
            // Reoriented Normal Mapping (RNM) is best, but simple addition/normalization works for small perturbations.
            // Or just: viewNormal = normalize(viewNormal + vec3(viewPerturb.xy, 0.0));
            // Better: Rotate viewNormal by the rotation defined by viewPerturb?
            // Simple approach: Mix based on deformation factor?
            // No, the perturbation IS the geometry.
            // We should apply it.
            // But we need to combine it with the underlying terrain normal (hills).
            // 'viewNormal' contains the hill normal (and normal map).
            // 'viewPerturb' contains the footprint shape (assuming flat ground).
            // We want to apply the footprint shape TO the hill.
            // Frame: T, B, N = viewNormal.
            // We want to tilt N by the local slope of the footprint.
            // This is exactly what Normal Mapping does.
            // Treat 'viewPerturb' as a tangent-space normal map where the "Tangent Plane" is defined by World(0,0,1).
            // But our base surface is 'viewNormal'.
            // Let's just add the offsets.
            // viewNormal.xy += viewPerturb.xy * 2.0; // Exaggerate slightly?
            // viewNormal = normalize(viewNormal);
            
            // Let's try blending.
            // Since viewPerturb is (0,0,1) when flat.
            // We can treat it as a detail normal.
            // UDN Blending: n = normalize(n1 + n2). (If n1, n2 are roughly Z-up).
            // But viewNormal might not be Z-up (steep hill).
            // However, footprints are usually on flat-ish ground.
            // Let's try simple addition.
            
            // Remove the Z component from perturb to get just the "tilt"
            vec3 tilt = viewPerturb - vec3(0,0,1); // Assuming viewPerturb is mostly Z-up in View Space? 
            // No, View Space Z is towards camera.
            // World Space Z is Up.
            // gl_NormalMatrix * (0,0,1) -> View Space Up.
            // Let's call it viewUp = gl_NormalMatrix * vec3(0,0,1).
            // The perturbation is relative to World Up.
            // We want to apply the *difference* between viewPerturb and viewUp to viewNormal.
            
            vec3 viewUp = normalize(gl_NormalMatrix * vec3(0,0,1.0));
            vec3 diff = viewPerturb - viewUp;
            
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
