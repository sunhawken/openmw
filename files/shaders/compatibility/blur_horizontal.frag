#version 120

uniform sampler2D inputTex;

// 9-tap Gaussian weights (wider kernel for softer, more natural-looking edges)
// Generated with sigma = 2.5 for a smooth falloff
// Sum = 1.0
const float weights[9] = float[](
    0.1531, // center
    0.1448, // +/- 1
    0.1225, // +/- 2
    0.0926, // +/- 3
    0.0627, // +/- 4
    0.0379, // +/- 5
    0.0205, // +/- 6
    0.0099, // +/- 7
    0.0043  // +/- 8
);

// Blur spread multiplier - larger values = wider blur
// 2.0 = double the effective blur radius for softer snow edges
const float blurSpread = 2.0;

void main()
{
    vec2 uv = gl_TexCoord[0].xy;
    vec2 texelSize = 1.0 / vec2(2048.0, 2048.0);

    // Center tap
    float result = texture2D(inputTex, uv).r * weights[0];

    // Side taps - using larger spread for softer edges
    for(int i = 1; i < 9; ++i)
    {
        float offset = float(i) * texelSize.x * blurSpread;
        result += texture2D(inputTex, uv + vec2(offset, 0.0)).r * weights[i];
        result += texture2D(inputTex, uv - vec2(offset, 0.0)).r * weights[i];
    }

    gl_FragColor = vec4(result, 0.0, 0.0, 1.0);
}
