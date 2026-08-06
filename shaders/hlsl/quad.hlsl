cbuffer QuadConstants : register(b0)
{
    float4 rect;      // x, y, w, h in pixels
    float4 color;     // rgba
    float2 viewport;  // framebuffer size in pixels
    float2 padding;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color    : COLOR0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 corner = float2(float(vertexId & 1u), float((vertexId >> 1u) & 1u));
    float2 pixel  = rect.xy + corner * rect.zw;

    VSOutput output;
    // Pixel space is top-left origin with +Y down; D3D12 clip space is Y-up.
    output.position = float4(pixel.x / viewport.x * 2.0 - 1.0,
                             1.0 - pixel.y / viewport.y * 2.0,
                             0.0,
                             1.0);
    output.color = color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return input.color;
}
