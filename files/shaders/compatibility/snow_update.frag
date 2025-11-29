#version 120

uniform sampler2D previousFrame; // The accumulation buffer from the previous frame
uniform sampler2D objectMask;    // The mask of objects currently touching the ground (White = Object)
uniform vec2 offset;             // UV offset for sliding window (scrolling)
uniform float decayAmount;       // Amount to decay per frame

void main()
{
    // 1. Calculate UV for reading from the previous frame
    // We are rendering a fullscreen quad [0,1].
    // If the player moved, the "ground" moved relative to our window.
    // We need to sample the previous frame at the OLD position of this pixel.
    // UV_old = UV_new + Offset
    vec2 uv = gl_TexCoord[0].xy;
    vec2 oldUV = uv + offset;

    // 2. Sample Previous Frame (with bounds check)
    float previousValue = 0.0;
    if (oldUV.x >= 0.0 && oldUV.x <= 1.0 && oldUV.y >= 0.0 && oldUV.y <= 1.0)
    {
        previousValue = texture2D(previousFrame, oldUV).r;
    }

    // 3. Apply Decay
    // Decay reduces the deformation over time (snow fills back in)
    previousValue = max(0.0, previousValue - decayAmount);

    // 4. Sample Object Mask (New Deformation)
    // The object mask is rendered from below. White means an object is there.
    // We assume the mask is aligned with the CURRENT frame's view.
    float newValue = texture2D(objectMask, uv).r;

    // 5. Combine
    // Accumulate: Keep the deeper deformation (max of previous and new)
    float finalValue = max(previousValue, newValue);

    // 6. Cubic Remapping (Rim Effect)
    // DEBUG: Disable rim for now
    // float rimIntensity = 2.0; 
    // finalValue = finalValue - rimIntensity * finalValue * (1.0 - finalValue);

    gl_FragColor = vec4(finalValue, 0.0, 0.0, 1.0);
}
