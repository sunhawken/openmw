#version 120

uniform sampler2D inputTex;

// 5-tap Gaussian weights
// 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216
const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main()
{
    vec2 uv = gl_TexCoord[0].xy;
    vec2 texelSize = 1.0 / vec2(2048.0, 2048.0); // Hardcoded for now, or use textureSize in newer GLSL

    // Center tap
    float result = texture2D(inputTex, uv).r * weights[0];

    // Side taps
    for(int i = 1; i < 5; ++i)
    {
        float offset = float(i) * texelSize.x;
        result += texture2D(inputTex, uv + vec2(offset, 0.0)).r * weights[i];
        result += texture2D(inputTex, uv - vec2(offset, 0.0)).r * weights[i];
    }

    gl_FragColor = vec4(result, 0.0, 0.0, 1.0);
}
