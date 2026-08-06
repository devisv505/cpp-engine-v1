#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "world/TileRegistry.h"

namespace engine {

// GPU-ready tile rendering data, built once after the registry freezes:
//
//  - atlas:   RGBA8 image holding every tile texture side by side. Untextured
//             setups get a 1x1 white pixel so backends always have a texture.
//  - palette: kMaxTileTypes x 2 RGBA float32 image. Row 0 = tile color (rgb)
//             + has-texture flag (a). Row 1 = the tile's atlas UV rect
//             (u0, v0, u1, v1). The tile shader looks both up by tile id.
//
// Texture paths resolve through ResolveDataPath, so a development build can
// load them straight from the source tree.
struct TileRenderData {
    std::vector<uint8_t> atlasPixels;  // RGBA8, atlasWidth * atlasHeight * 4
    int                  atlasWidth  = 1;
    int                  atlasHeight = 1;

    std::vector<float> palettePixels;  // RGBA32F, kMaxTileTypes * 2 * 4
};

// Loads every prototype's texture (writing UV rects back into the registry),
// packs the atlas, and fills the palette. Missing or broken image files log
// a warning and leave that tile untextured — this never fails the app.
TileRenderData BuildTileRenderData(TileRegistry& registry);

} // namespace engine
