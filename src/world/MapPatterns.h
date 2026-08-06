#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "world/TileMap.h"
#include "world/TileRegistry.h"

namespace engine {

// Parameters a pattern may use; Lua fills the relevant ones. Patterns run
// entirely in C++ — Lua only names the pattern and provides these values.
struct PatternParams {
    std::vector<TileId> tiles;         // participating tile ids, in order
    std::vector<float>  weights;       // random: optional, parallel to tiles
    int                 cellSize = 1;  // checkerboard: tiles per square
    uint32_t            seed     = 0;  // random: 0 = engine default
};

// Fills the whole map. Returns false for an unknown pattern name.
// Known patterns: "checkerboard", "random", "solid".
bool GenerateMapPattern(TileMap& map, const std::string& pattern, const PatternParams& params);

// Names for UI listing, in a stable order.
const std::vector<std::string>& KnownPatternNames();

} // namespace engine
