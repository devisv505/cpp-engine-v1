#version 450

// Mirrors engine::QuadConstants (64 bytes) exactly; padding keeps the block the
// same size as the MSL and HLSL versions.
layout(push_constant) uniform QuadConstants {
    vec4 rect;      // x, y, w, h in pixels
    vec4 color;     // rgba
    vec2 viewport;  // framebuffer width, height in pixels
    vec2 padding;
} quad;

layout(location = 0) out vec4 vColor;

void main()
{
    // Triangle-strip corner order: (0,0) (1,0) (0,1) (1,1).
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vec2 pixel  = quad.rect.xy + corner * quad.rect.zw;

    // Vulkan clip space has +Y pointing down, which is already the direction
    // quads are expressed in, so Y maps across without a flip (unlike Metal
    // and D3D12, where it has to be inverted).
    gl_Position = vec4(pixel.x / quad.viewport.x * 2.0 - 1.0,
                       pixel.y / quad.viewport.y * 2.0 - 1.0,
                       0.0,
                       1.0);

    vColor = quad.color;
}
