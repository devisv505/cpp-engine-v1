cbuffer LightConstants : register(b0)
{
    float2 lightPos;       // world pixels
    float2 lightDir;       // normalized beam direction
    float4 lightColor;     // rgb premultiplied by intensity
    float  lightDistance;  // beam length, world pixels
    float  cosHalfAngle;
    float  softness;       // 0 hard edge .. 1 fully feathered
    float  mode;           // 0 = cone, 1 = screen-space god rays
    float2 camera;         // world pixels at the viewport center
    float  zoom;           // screen pixels per world pixel
    float  pad0;
    float2 viewport;       // framebuffer size in pixels
    float2 pad1;
    float4 maskRect;       // mask origin xy, mask size zw, in world pixels
};

Texture2D<float4> occlusionMask : register(t0);
SamplerState      linearClamp   : register(s0);

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

float Hash12(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

// Interleaved gradient noise: much more even than a white-noise hash at low
// sample counts, so the residual dither reads as fine grain, not clumps.
float DitherIGN(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// World position -> mask UV. Anything outside the mask counts as unoccluded.
float OcclusionAt(float2 world)
{
    float2 uv = (world - maskRect.xy) / maskRect.zw;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
        return 0.0;
    }
    return occlusionMask.SampleLevel(linearClamp, uv, 0).r;
}

float4 PSMain(VSOutput input) : SV_Target
{
    // SV_Position is top-left-origin with +Y down, which matches world space,
    // so there are no axis flips here.
    float2 worldPx = camera + (input.position.xy - viewport * 0.5) / zoom;
    float2 toFrag  = worldPx - lightPos;
    float  dist    = length(toFrag);
    if (dist > lightDistance) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float radial = 1.0 - dist / lightDistance;
    radial *= radial;
    // Per-pixel dither on the march start hides the stepping as banding.
    float dither = DitherIGN(input.position.xy);

    if (mode > 0.5) {
        // God rays: march from the fragment toward the light, accumulating
        // unoccluded samples with exponential decay.
        const int RAY_STEPS = 32;
        float shaft = 0.0;
        float decay = 1.0;
        [loop] for (int i = 0; i < RAY_STEPS; ++i) {
            float  t = (float(i) + dither) / float(RAY_STEPS);
            float2 p = lerp(worldPx, lightPos, t);
            shaft += (1.0 - OcclusionAt(p)) * decay;
            decay *= 0.96;
        }
        shaft /= float(RAY_STEPS);
        return float4(lightColor.rgb * shaft * radial, 1.0);
    }

    float2 dirToFrag = dist > 1e-4 ? toFrag / dist : lightDir;
    float  cosA  = dot(dirToFrag, lightDir);
    float  outer = cosHalfAngle;
    float  inner = lerp(1.0, outer, 1.0 - softness * 0.999);
    // softness 0 collapses inner onto outer, and smoothstep is undefined when
    // its two edges are equal; nudging keeps the hard-edge case well-defined.
    inner = max(inner, outer + 1e-4);
    float  cone  = smoothstep(outer, inner, cosA);
    if (cone <= 0.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // March from the light toward the fragment; each occluded sample eats 90%
    // of the remaining visibility.
    // Step at roughly the occlusion mask's texel size (4 world px) so walls
    // thinner than one step cannot be skipped, bounded for long rays.
    int coneSteps = (int)clamp(dist / 4.0, 16.0, 96.0);

    // Optical depth accumulates DISTANCE travelled through occluders, so the
    // result converges as the step count rises instead of drifting with it,
    // and one extra sample shifts brightness smoothly rather than tenfold.
    float stepLength = dist / (float)coneSteps;
    float depth = 0.0;
    [loop] for (int j = 1; j <= coneSteps; ++j) {
        float t = (float(j) - dither) / (float)coneSteps;
        depth += OcclusionAt(lightPos + toFrag * t) * stepLength;
    }
    // 0.25 per world pixel: a half-tile (16 px) wall transmits ~2%.
    float vis = exp(-depth * 0.25);

    return float4(lightColor.rgb * cone * radial * vis, 1.0);
}
