#include <metal_stdlib>
using namespace metal;

struct TileConstants {
    float2 camera;
    float  zoom;
    float  tileSizePx;
    float2 viewport;
    float2 mapSize;
    float4 background;
    float4 padding;
};

vertex float4 tile_vertex(uint vertexId [[vertex_id]])
{
    // Fullscreen triangle: (-1,-1) (3,-1) (-1,3).
    float2 corner = float2(float((vertexId << 1) & 2u), float(vertexId & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

fragment float4 tile_fragment(float4 position [[position]],
                              constant TileConstants& c [[buffer(0)]],
                              texture2d<uint>  tileIds [[texture(0)]],
                              texture2d<float> atlas   [[texture(1)]],
                              texture2d<float> palette [[texture(2)]])
{
    float2 worldPx  = c.camera + (position.xy - c.viewport * 0.5) / c.zoom;
    float2 tilePos  = floor(worldPx / c.tileSizePx);
    if (tilePos.x < 0.0 || tilePos.y < 0.0 ||
        tilePos.x >= c.mapSize.x || tilePos.y >= c.mapSize.y) {
        return c.background;
    }

    uint id = min(tileIds.read(uint2(tilePos)).r, 255u);
    float4 colorRow = palette.read(uint2(id, 0));
    float4 uvRow    = palette.read(uint2(id, 1));

    float3 result = colorRow.rgb;
    if (colorRow.a > 0.5) {
        constexpr sampler pointClamp(filter::nearest, address::clamp_to_edge);
        float2 f  = fract(worldPx / c.tileSizePx);
        float2 uv = mix(uvRow.xy, uvRow.zw, f);
        result = atlas.sample(pointClamp, uv).rgb * colorRow.rgb;
    }
    return float4(result, 1.0);
}
