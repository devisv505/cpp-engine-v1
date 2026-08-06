#include <metal_stdlib>
using namespace metal;

struct QuadConstants {
    float4 rect;      // x, y, w, h in pixels
    float4 color;
    float2 viewport;  // framebuffer size in pixels
    float2 padding;
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

vertex VertexOut quad_vertex(uint vertexId [[vertex_id]],
                             constant QuadConstants& constants [[buffer(0)]])
{
    float2 corner = float2(float(vertexId & 1u), float((vertexId >> 1) & 1u));
    float2 pixel  = constants.rect.xy + corner * constants.rect.zw;

    VertexOut out;
    out.position = float4(pixel.x / constants.viewport.x * 2.0 - 1.0,
                          1.0 - pixel.y / constants.viewport.y * 2.0,
                          0.0, 1.0);
    out.color = constants.color;
    return out;
}

fragment float4 quad_fragment(VertexOut in [[stage_in]])
{
    return in.color;
}
