#version 120

uniform sampler2D previousFrame; // The accumulation buffer from the previous frame
uniform sampler2D objectMask;    // The mask of objects currently touching the ground (White = Object)
uniform vec2 offset;             // UV offset for sliding window (scrolling)
uniform float decayAmount;       // Amount to decay per frame
uniform bool firstFrame;         // Reset accumulation on first frame

void main()
{
    vec2 uv = gl_TexCoord[0].xy;

    // 1. Calculate UV for reading from the previous frame (with sliding window offset)
    vec2 oldUV = uv + offset;

    // 2. Sample Previous Frame (with bounds check)
    float previousValue = 0.0;
    if (!firstFrame && oldUV.x >= 0.0 && oldUV.x <= 1.0 && oldUV.y >= 0.0 && oldUV.y <= 1.0)
    {
        previousValue = texture2D(previousFrame, oldUV).r;
    }

    // 3. Apply Decay - snow fills back in over time
    previousValue = max(0.0, previousValue - decayAmount);

    // 4. Sample Object Mask (current frame deformation)
    float newValue = texture2D(objectMask, uv).r;

    // 5. Combine - keep the deeper deformation (max of previous and new)
    // Output raw 0-1 values; the rim function will be applied AFTER blur
    // in blur_vertical.frag where we have gradient information
    float finalValue = max(previousValue, newValue);

    gl_FragColor = vec4(finalValue, 0.0, 0.0, 1.0);
}
