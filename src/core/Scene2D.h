#pragma once

#include <vector>

namespace engine {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// A rectangle in 2D pixel space: origin is the top-left of the window,
// +X points right, +Y points down. Backends convert to their own clip space.
struct Quad {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    Color color;
};

// The drawable contents of a frame. Built by Lua, consumed by the renderer.
struct Scene2D {
    Color             clearColor{0.08f, 0.09f, 0.12f, 1.0f};
    std::vector<Quad> quads;

    void Reset()
    {
        clearColor = Color{0.08f, 0.09f, 0.12f, 1.0f};
        quads.clear();
    }
};

// Per-draw constants handed to every backend's quad shader. The layout is
// shared by MSL, HLSL, and GLSL: 64 bytes, small enough for push/root constants.
struct QuadConstants {
    float rect[4];      // x, y, w, h in pixels
    float color[4];     // rgba
    float viewport[2];  // framebuffer width, height in pixels
    float padding[2];
};

// Per-light constants for the volumetric passes (52 bytes). Backends combine
// these fields with camera and occlusion-mask bounds into their own padded
// push/root-constant block, so this struct describes the light data only.
struct LightDrawConstants {
    float positionX, positionY;   // world pixels
    float dirX, dirY;             // normalized beam direction
    float color[4];               // rgb premultiplied by intensity, a unused
    float distance;               // beam length in world pixels
    float cosHalfAngle;           // cone half-angle, precomputed cosine
    float softness;               // 0 hard edge .. 1 fully feathered
    float mode;                   // 0 = cone, 1 = screen-space light
    float pixelArt;               // 0 = smooth, 1 = pixel-art style
};

// Constants for the tile-map pass; same 64-byte cross-backend contract.
// The tile shader runs as one fullscreen draw: each fragment computes which
// world tile it covers from the camera, fetches the tile id from the id
// texture, and resolves color/texture through the palette texture.
struct TileDrawConstants {
    float cameraX, cameraY;        // world position (in world pixels) at viewport center
    float zoom;                    // screen pixels per world pixel
    float tileSizePx;              // world pixels per tile
    float viewportW, viewportH;    // framebuffer size in pixels
    float mapWidth, mapHeight;     // map size in tiles
    float background[4];           // color outside the map bounds
    float padding[4];
};

} // namespace engine
