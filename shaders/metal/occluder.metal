#include <metal_stdlib>
using namespace metal;

struct OccluderConstants {
    float4 rect;       // wall x, y, w, h in world pixels
    float4 maskRect;   // mask origin x, y, size w, h in world pixels
};

vertex float4 occluder_vertex(uint vertexId [[vertex_id]],
                              constant OccluderConstants& c [[buffer(0)]])
{
    float2 corner = float2(float(vertexId & 1u), float((vertexId >> 1) & 1u));
    float2 world  = c.rect.xy + corner * c.rect.zw;
    float2 uv     = (world - c.maskRect.xy) / c.maskRect.zw;
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

fragment float occluder_fragment()
{
    return 1.0;
}
