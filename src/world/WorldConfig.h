#pragma once

#include <string>
#include <vector>

#include "core/math/Color.h"
#include "world/MapPatterns.h"

namespace engine {

// Camera/editor settings a script may override via the global `editor` table.
struct EditorConfig {
    float panSpeed = 900.0f;   // screen pixels per second
    float zoomMin  = 0.125f;
    float zoomMax  = 8.0f;
    // SDL key names ("W", "Up", ...); resolved with SDL_GetKeyFromName.
    std::string keyUp = "W", keyDown = "S", keyLeft = "A", keyRight = "D";
};

// Everything the Lua script configures about the world. Tile prototypes are
// added straight into the TileRegistry while parsing; pattern tile names are
// resolved to ids at the same time.
struct WorldConfig {
    std::string   pattern = "checkerboard";
    int           width   = 256;
    int           height  = 256;
    PatternParams params;
    Color         background{0.05f, 0.05f, 0.06f, 1.0f};
    EditorConfig  editor;
};

} // namespace engine
