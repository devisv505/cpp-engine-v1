#version 450

// Mirrors engine::TileDrawConstants (64 bytes) exactly.
layout(push_constant) uniform TileDrawConstants {
    vec2 camera;       // world position (in world pixels) at the viewport center
    float zoom;        // screen pixels per world pixel
    float tileSizePx;  // world pixels per tile
    vec2 viewport;     // framebuffer size in pixels
    vec2 mapSize;      // map size in tiles
    vec4 background;   // color outside the map bounds
    vec4 padding;
} tile;

layout(set = 0, binding = 0) uniform usampler2D tileIds;  // R16_UINT, one texel per tile
layout(set = 0, binding = 1) uniform sampler2D atlas;     // RGBA8 tile textures
layout(set = 0, binding = 2) uniform sampler2D palette;   // RGBA32F, 256x2 lookup

layout(location = 0) out vec4 outColor;

void main()
{
    // gl_FragCoord has a top-left origin with +Y down, which is already the
    // world-space convention, so the mapping needs no flip.
    vec2 worldPx = tile.camera + (gl_FragCoord.xy - tile.viewport * 0.5) / tile.zoom;
    vec2 tilePos = floor(worldPx / tile.tileSizePx);

    if (tilePos.x < 0.0 || tilePos.y < 0.0 ||
        tilePos.x >= tile.mapSize.x || tilePos.y >= tile.mapSize.y) {
        outColor = tile.background;
        return;
    }

    uint id = min(texelFetch(tileIds, ivec2(tilePos), 0).r, 255u);

    // Palette row 0 = tile color (rgb) + has-texture flag (a);
    // row 1 = the tile's atlas UV rect (u0, v0, u1, v1).
    vec4 colorRow = texelFetch(palette, ivec2(int(id), 0), 0);
    vec4 uvRow    = texelFetch(palette, ivec2(int(id), 1), 0);

    vec3 result = colorRow.rgb;
    if (colorRow.a > 0.5) {
        vec2 f  = fract(worldPx / tile.tileSizePx);
        vec2 uv = mix(uvRow.xy, uvRow.zw, f);
        result  = texture(atlas, uv).rgb * colorRow.rgb;
    }
    outColor = vec4(result, 1.0);
}
