#pragma once

#include <cstddef>

namespace engine {

// GPU constant-buffer layouts, matched field for field by the MSL, HLSL and
// GLSL shaders. They live with the renderer rather than in core because they
// are the backends' ABI: a change here is a change to every shader.
//
// Watch alignment when editing. A vec4/float4 must sit on a 16-byte boundary in
// all three shading languages, so reordering these fields silently changes what
// the shader reads. Keep the sizes and field order in step with shaders/* --
// the static_asserts below enforce what the comments claim.

// Per-draw constants for the quad shader: 48 bytes, small enough for Metal
// vertex bytes, Vulkan push constants, or D3D12 root constants.
struct QuadConstants {
    float rect[4];      // x, y, w, h in pixels
    float color[4];     // rgba
    float viewport[2];  // framebuffer width, height in pixels
    float padding[2];
};

// Constants for the tile-map pass, 64 bytes; same cross-backend contract.
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

// The sizes are part of the contract with the shaders and with the backends'
// push/root-constant calls, so they are checked rather than merely documented.
static_assert(sizeof(QuadConstants) == 48, "QuadConstants must match the quad shaders");
static_assert(sizeof(TileDrawConstants) == 64, "TileDrawConstants must match the tile shaders");
static_assert(offsetof(QuadConstants, color) == 16, "float4 color must stay 16-byte aligned");
static_assert(offsetof(TileDrawConstants, background) == 32,
              "float4 background must stay 16-byte aligned");

} // namespace engine
