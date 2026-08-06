#pragma once

#include <string>

#include "world/TileMap.h"
#include "world/TileRegistry.h"

namespace engine {

// Binary map format "CMAP", version 1:
//   magic u32 | version u32 | width i32 | height i32 | nameCount u32
//   names: (len u32, bytes) per tile id used at save time, in id order
//   tiles: width*height u16, row-major
//
// Saves record prototype *names*, so a map still loads after the Lua tile
// list is reordered; tiles whose name no longer exists load as void (id 0)
// with a warning.
bool SaveMap(const TileMap& map, const TileRegistry& registry, const std::string& path);
bool LoadMap(TileMap& map, const TileRegistry& registry, const std::string& path);

} // namespace engine
