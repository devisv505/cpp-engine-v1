#version 450

// Rasterizes one wall into the world-space occlusion mask. The mask's own world
// rectangle turns the wall's world pixels into clip space, so the draw needs no
// vertex buffer and no CPU-side transform: 4 vertices, one triangle strip.
layout(push_constant) uniform OccluderConstants {
    vec4 rect;      // wall x, y, w, h in world pixels
    vec4 maskRect;  // mask origin x, y and size w, h in world pixels
} occluder;

void main()
{
    // Triangle-strip corner order: (0,0) (1,0) (0,1) (1,1).
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vec2 world  = occluder.rect.xy + corner * occluder.rect.zw;

    // Mask texel (0,0) is the world rect's top-left corner. Vulkan clip space
    // has +Y pointing down, which is already the world-space convention, so Y
    // maps across without a flip and the light shaders can sample with the same
    // (world - origin) / size mapping.
    vec2 uv = (world - occluder.maskRect.xy) / occluder.maskRect.zw;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
