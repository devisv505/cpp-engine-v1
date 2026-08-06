#version 450

// One visible light, accumulated with additive blending (src ONE, dst ONE), so
// writing zero is the same as not covering the pixel at all.
//
// Mirrors VulkanRenderer's LightPushConstants: engine::LightDrawConstants alone
// carries no camera, so the light fields and the camera/mask fields share one
// 96-byte block -- comfortably inside the 128 bytes of push-constant space
// every Vulkan implementation guarantees.
//
//    0  vec2  position      light position, world pixels
//    8  vec2  direction     normalized beam direction
//   16  vec4  color         rgb premultiplied by intensity, a unused
//   32  float range         beam length, world pixels
//   36  float cosHalfAngle  cone half-angle, precomputed cosine
//   40  float softness      0 hard edge .. 1 fully feathered
//   44  float mode          0 = volumetric cone, 1 = screen-space light
//   48  vec2  camera        world position at the viewport center
//   56  float zoom          screen pixels per world pixel
//   60  float pixelArt      0 = smooth, 1 = pixel-art style
//   64  vec2  viewport      framebuffer size in pixels
//   72  vec2  maskOrigin    world pixels of the mask's top-left corner
//   80  vec2  maskSize      world pixels the mask spans
//   88  vec2  pad1          tail padding -- 96 bytes total
layout(push_constant) uniform LightConstants {
    vec2  position;
    vec2  direction;
    vec4  color;
    float range;
    float cosHalfAngle;
    float softness;
    float mode;
    vec2  camera;
    float zoom;
    float pixelArt;
    vec2  viewport;
    vec2  maskOrigin;
    vec2  maskSize;
    vec2  pad1;
} light;

// R8_UNORM world-space mask: 1 blocks light, 0 lets it through.
layout(set = 0, binding = 0) uniform sampler2D occlusionMask;

layout(location = 0) out vec4 outColor;

// Cheap per-pixel hash. Dithering the march start turns the fixed step count
// into noise instead of visible banding.
float Hash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// Interleaved gradient noise: much more even than a white-noise hash at low
// sample counts, so the residual dither reads as fine grain, not clumps.
float DitherIGN(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float PixelLevel(float value)
{
    const float levels = 6.0;
    return floor(clamp(value, 0.0, 1.0) * levels + 0.5) / levels;
}

// 0 outside the mask: the world beyond it simply has no occluders recorded.
// textureLod, not texture: the sampling below sits in non-uniform control flow,
// where implicit derivatives are undefined, and the mask has a single mip anyway.
float Occlusion(vec2 worldPx)
{
    vec2 uv = (worldPx - light.maskOrigin) / light.maskSize;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 0.0;
    }
    return textureLod(occlusionMask, uv, 0.0).r;
}

// gl_FragCoord has a top-left origin with +Y down, which is already the
// world-space convention, so the mapping needs no flip (same as tile.frag).
vec2 ScreenToWorld(vec2 screenPx)
{
    return light.camera + (screenPx - light.viewport * 0.5) / light.zoom;
}

void main()
{
    bool  pixelated = light.pixelArt > 0.5;
    vec2  worldPx = ScreenToWorld(gl_FragCoord.xy);
    if (pixelated) {
        const float cellSize = 4.0;
        worldPx = floor(worldPx / cellSize) * cellSize + cellSize * 0.5;
    }
    vec2  toFrag  = worldPx - light.position;
    float dist    = length(toFrag);

    if (dist > light.range) {
        outColor = vec4(0.0);
        return;
    }

    float radial = 1.0 - dist / light.range;  // linear falloff ...
    radial *= radial;                         // ... squared, so the rim fades out
    // A fixed midpoint keeps pixel-art blocks stable; smooth lights retain
    // per-pixel jitter to hide ray-march bands.
    float dither = pixelated ? 0.5 : DitherIGN(gl_FragCoord.xy);

    if (light.mode > 0.5) {
        // Screen-space lights are omnidirectional, but their visibility is
        // still line-of-sight. Integrating blocker depth prevents open samples
        // on either side of a wall from incorrectly lighting pixels behind it.
        int steps = int(clamp(dist / 4.0, 16.0, 96.0));
        float stepLength = dist / float(steps);
        float depth = 0.0;
        for (int i = 1; i <= steps; ++i) {
            float t = (float(i) - dither) / float(steps);
            depth += Occlusion(light.position + toFrag * t) * stepLength;
        }
        float visibility = exp(-depth * 0.25);
        float amount = visibility * radial;
        amount = pixelated ? PixelLevel(amount) : amount;
        outColor = vec4(light.color.rgb * amount, 1.0);
        return;
    }

    // Cone: angular falloff feathered by softness.
    vec2  dirToFrag = dist > 1e-4 ? toFrag / dist : light.direction;
    float cosA      = dot(dirToFrag, light.direction);
    float outer     = light.cosHalfAngle;
    float inner     = mix(1.0, outer, 1.0 - light.softness * 0.999);
    // softness 0 collapses inner onto outer, and smoothstep is undefined when
    // its edges are equal; the epsilon keeps it a well-defined hard edge.
    inner = max(inner, outer + 1e-4);
    float cone = smoothstep(outer, inner, cosA);
    if (cone <= 0.0) {
        outColor = vec4(0.0);
        return;
    }

    // Step at roughly the occlusion mask's texel size (4 world px) so walls
    // thinner than one step cannot be skipped, bounded for long rays.
    int steps = int(clamp(dist / 4.0, 16.0, 96.0));

    // Optical depth accumulates DISTANCE travelled through occluders, so the
    // result converges as the step count rises instead of drifting with it,
    // and one extra sample shifts brightness smoothly rather than tenfold.
    float stepLength = dist / float(steps);
    float depth = 0.0;
    for (int i = 1; i <= steps; ++i) {
        float t = (float(i) - dither) / float(steps);
        depth += Occlusion(light.position + toFrag * t) * stepLength;
    }
    // 0.25 per world pixel: a half-tile (16 px) wall transmits ~2%.
    float visibility = exp(-depth * 0.25);

    float amount = cone * radial * visibility;
    amount = pixelated ? PixelLevel(amount) : amount;
    outColor = vec4(light.color.rgb * amount, 1.0);
}
