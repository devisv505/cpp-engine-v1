cbuffer OccluderConstants : register(b0)
{
    float4 rect;      // wall x, y, w, h in world pixels
    float4 maskRect;  // mask origin xy, mask size zw, in world pixels
};

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 corner = float2(float(vertexId & 1u), float((vertexId >> 1u) & 1u));
    float2 world  = rect.xy + corner * rect.zw;
    float2 uv     = (world - maskRect.xy) / maskRect.zw;
    // The mask is world-space with +Y down, so uv.y = 0 is the top row of the
    // texture; D3D clip space is Y-up, hence the flip here and nowhere else.
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

float4 PSMain() : SV_Target
{
    return float4(1.0, 1.0, 1.0, 1.0);  // R8_UNORM target: only .r survives
}
