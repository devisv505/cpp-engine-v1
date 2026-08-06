#include <metal_stdlib>
using namespace metal;

// float4 members must sit on 16-byte boundaries, so both vectors lead and the
// scalars follow. The C++ side writes this exact 96-byte layout.
struct LightConstants {
    float4 lightColor;    //  0: rgb premultiplied by intensity
    float4 maskRect;      // 16: mask origin xy, size zw (world pixels)
    float2 lightPos;      // 32: world pixels
    float2 lightDir;      // 40: normalized
    float  lightDistance; // 48
    float  cosHalfAngle;  // 52
    float  softness;      // 56
    float  mode;          // 60: 0 = cone, 1 = god rays
    float2 camera;        // 64: world pixels at viewport centre
    float  zoom;          // 72
    float  pad0;          // 76
    float2 viewport;      // 80
    float2 pad1;          // 88
};

vertex float4 fullscreen_vertex(uint vertexId [[vertex_id]])
{
    float2 corner = float2(float((vertexId << 1) & 2u), float(vertexId & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

// Interleaved gradient noise: far more even than a white-noise hash at low
// sample counts, so the residual dither reads as fine grain instead of clumps.
static inline float dither_ign(float2 p)
{
    return fract(52.9829189 * fract(dot(p, float2(0.06711056, 0.00583715))));
}

static inline float hash12(float2 p)
{
    return fract(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

static inline float sample_mask(texture2d<float> mask, sampler s, float2 world, float4 maskRect)
{
    float2 uv = (world - maskRect.xy) / maskRect.zw;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
        return 0.0;
    }
    return mask.sample(s, uv).r;
}

fragment float4 light_fragment(float4 position [[position]],
                               constant LightConstants& c [[buffer(0)]],
                               texture2d<float> mask [[texture(0)]])
{
    constexpr sampler linearClamp(filter::linear, address::clamp_to_edge);

    float2 worldPx = c.camera + (position.xy - c.viewport * 0.5) / c.zoom;
    float2 toFrag  = worldPx - c.lightPos;
    float  dist    = length(toFrag);
    if (dist > c.lightDistance) {
        return float4(0.0);
    }

    float radial = 1.0 - dist / c.lightDistance;
    radial *= radial;
    float dither = dither_ign(position.xy);

    if (c.mode > 0.5) {
        // God rays: march from the fragment toward the light in screen space,
        // accumulating unoccluded samples with exponential decay.
        const int STEPS = 32;
        float shaft = 0.0;
        float decay = 1.0;
        for (int i = 0; i < STEPS; ++i) {
            float t = (float(i) + dither) / float(STEPS);
            float2 p = mix(worldPx, c.lightPos, t);
            shaft += (1.0 - sample_mask(mask, linearClamp, p, c.maskRect)) * decay;
            decay *= 0.96;
        }
        shaft /= float(STEPS);
        return float4(c.lightColor.rgb * shaft * radial, 1.0);
    }

    float2 dirToFrag = dist > 1e-4 ? toFrag / dist : c.lightDir;
    float  cosA  = dot(dirToFrag, c.lightDir);
    float  outer = c.cosHalfAngle;
    float  inner = mix(1.0, outer, 1.0 - c.softness * 0.999);
    // softness 0 collapses inner onto outer, and smoothstep is undefined when
    // its two edges are equal; nudging keeps the hard-edge case well-defined.
    inner = max(inner, outer + 1e-4);
    float  cone  = smoothstep(outer, inner, cosA);
    if (cone <= 0.0) {
        return float4(0.0);
    }

    // Step at roughly the mask's texel size (4 world px) so walls thinner than
    // a step cannot be skipped, bounded so long rays stay affordable.
    const float MASK_TEXEL = 4.0;
    int steps = int(clamp(dist / MASK_TEXEL, 16.0, 96.0));

    // Beer-Lambert: accumulate optical depth, then transmit. Unlike multiplying
    // by 0.1 per hit, one extra sample shifts the result smoothly, so the
    // dither produces fine grain rather than a tenfold brightness flip.
    // Optical depth accumulates DISTANCE travelled through occluders, so the
    // result converges as the step count rises instead of drifting with it.
    float stepLength = dist / float(steps);
    float depth = 0.0;
    for (int i = 1; i <= steps; ++i) {
        float t = (float(i) - dither) / float(steps);
        float2 p = c.lightPos + toFrag * t;
        depth += sample_mask(mask, linearClamp, p, c.maskRect) * stepLength;
    }
    // 0.25 per world pixel: a half-tile (16 px) wall transmits ~2%.
    float vis = exp(-depth * 0.25);

    return float4(c.lightColor.rgb * cone * radial * vis, 1.0);
}

