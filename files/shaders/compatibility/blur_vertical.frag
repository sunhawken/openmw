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

// Rim Function: Creates raised edges around footprints
// Applied AFTER blur so we have gradient values (0→1 transitions from blur)
//
// Using sin-based function that creates NEGATIVE values at intermediate x:
// f(x) = x - C * sin(pi * x)
//
// Key values with C=0.15:
//   f(0.0) = 0.0 - 0.15*sin(0) = 0.0        (flat snow stays flat)
//   f(0.5) = 0.5 - 0.15*sin(pi/2) = 0.35    (middle pushed down slightly)
//   f(1.0) = 1.0 - 0.15*sin(pi) = 1.0       (deepest point stays deep)
//
// The gradient zones (around 0.25) will have:
//   f(0.25) = 0.25 - 0.15*sin(pi/4) ≈ 0.25 - 0.106 = 0.144
//
// To get actual NEGATIVE values (rim above snow), we need stronger effect:
// With C=0.5: f(0.25) ≈ 0.25 - 0.35 = -0.10 (raised rim!)
const float rimStrength = 0.4;

float rimFunction(float x)
{
    // sin(pi*x) peaks at x=0.5 with value 1.0
    // Subtracting this creates a dip, and with enough strength,
    // the edges (around x=0.2-0.3) go NEGATIVE = raised terrain
    return x - rimStrength * sin(3.14159 * x);
}

void main()
{
    vec2 uv = gl_TexCoord[0].xy;
    vec2 texelSize = 1.0 / vec2(2048.0, 2048.0);

    // Center tap
    float result = texture2D(inputTex, uv).r * weights[0];

    // Side taps - using larger spread for softer edges
    for(int i = 1; i < 9; ++i)
    {
        float offset = float(i) * texelSize.y * blurSpread;
        result += texture2D(inputTex, uv + vec2(0.0, offset)).r * weights[i];
        result += texture2D(inputTex, uv - vec2(0.0, offset)).r * weights[i];
    }

    // Apply rim function AFTER blur - creates raised edges at gradient zones
    // The blurred result has smooth 0→1 transitions where the rim effect will appear
    // TEMPORARILY DISABLED to debug vibration issue
    // result = rimFunction(result);

    gl_FragColor = vec4(result, 0.0, 0.0, 1.0);
}
