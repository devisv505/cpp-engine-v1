cbuffer TileConstants : register(b0)
{
    float2 camera;      // world pixels at the viewport center
    float  zoom;        // screen pixels per world pixel
    float  tileSizePx;  // world pixels per tile
    float2 viewport;    // framebuffer size in pixels
    float2 mapSize;     // map size in tiles
    float4 background;  // color outside the map bounds
    float4 padding;
};

Texture2D<uint>   tileIds    : register(t0);
Texture2D         atlas      : register(t1);
Texture2D<float4> palette    : register(t2);
SamplerState      pointClamp : register(s0);

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    // One triangle that covers the whole viewport.
    const float2 corners[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };

    VSOutput output;
    output.position = float4(corners[vertexId], 0.0, 1.0);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    // SV_Position arrives as top-left-origin pixel coordinates with +Y down,
    // which matches world space, so there are no axis flips anywhere.
    float2 worldPx = camera + (input.position.xy - viewport * 0.5) / zoom;
    float2 tilePos = floor(worldPx / tileSizePx);

    if (any(tilePos < 0.0) || any(tilePos >= mapSize)) {
        return background;
    }

    uint   id       = min(tileIds.Load(int3(int2(tilePos), 0)), 255u);
    float4 colorRow = palette.Load(int3(int(id), 0, 0));  // rgb + has-texture flag
    float4 uvRow    = palette.Load(int3(int(id), 1, 0));  // atlas UV rect

    float3 result = colorRow.rgb;
    if (colorRow.a > 0.5) {
        float2 f  = frac(worldPx / tileSizePx);
        float2 uv = lerp(uvRow.xy, uvRow.zw, f);
        result = atlas.Sample(pointClamp, uv).rgb * colorRow.rgb;
    }
    return float4(result, 1.0);
}
