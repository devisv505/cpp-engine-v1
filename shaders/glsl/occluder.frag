#version 450

// Whatever a wall covers is fully opaque to light. The mask is R8_UNORM, so
// only the red channel is kept; the pass clears to 0 (unoccluded) first.
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(1.0);
}
