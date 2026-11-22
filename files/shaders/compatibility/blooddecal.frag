#version 120
uniform sampler2D diffuseMap;
varying vec2 bloodUV;
varying vec3 bloodViewPos;
void main()
{
    vec4 stain = texture2D(diffuseMap, bloodUV);
    stain.a *= clamp(gl_FrontMaterial.diffuse.a,0.0,1.0);
    // Source DecalModulate uses DST_COLOR + SRC_COLOR. RGB128 is
    // the neutral value in the original 8-bit textures. Respect alpha even
    // when transparent edge pixels contain saturated blood colours.
    vec3 modulation = mix(vec3(0.5), stain.rgb + vec3(0.5 - 128.0/255.0), stain.a);
#if @radialFog
    float dist = length(bloodViewPos);
#else
    float dist = abs(bloodViewPos.z);
#endif
#if @exponentialFog
    float fog = 1.0 - exp(-2.0 * max(0.0, dist - gl_Fog.start/2.0) / (gl_Fog.end-gl_Fog.start/2.0));
#else
    float fog = clamp((dist-gl_Fog.start)*gl_Fog.scale,0.0,1.0);
#endif
    // The already lit/fogged NPC supplies lighting. Distant decals approach
    // a neutral multiplier instead of painting the fog a second time.
    gl_FragData[0] = vec4(mix(modulation,vec3(0.5),fog),0.0);
}
