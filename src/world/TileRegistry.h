#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/math/Color.h"

namespace engine {

using TileId = uint16_t;

// A tile *type*, defined by Lua at startup and immutable afterwards
// (Factorio's prototype model). Rendering picks, per tile:
//   - solid color            (no texture)
//   - texture                (tint left at white)
//   - texture x tint         (both set)
struct TilePrototype {
    std::string name;
    Color       color{1.0f, 1.0f, 1.0f, 1.0f};  // solid color, or tint over texture
    std::string texturePath;                     // empty = untextured
    bool        walkable = true;                 // stored for future collision use

    // Filled by the atlas builder: normalized UV rect inside the tile atlas.
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    bool  hasTexture = false;
};

// Immutable after Freeze(). Ids are dense indices into the prototype list;
// id 0 always exists as the fallback "void" tile.
class TileRegistry {
public:
    static constexpr uint32_t kMaxTileTypes = 256;

    TileRegistry();

    // Returns the new tile's id, or 0 with a log error if the registry is
    // full or the name already exists.
    TileId Add(TilePrototype prototype);

    void Freeze() { m_frozen = true; }
    bool IsFrozen() const { return m_frozen; }

    TileId FindByName(const std::string& name) const;  // 0 when absent

    const TilePrototype& Get(TileId id) const;
    TilePrototype&       GetMutable(TileId id);  // atlas builder fills UVs
    uint32_t             Count() const { return static_cast<uint32_t>(m_prototypes.size()); }

private:
    std::vector<TilePrototype> m_prototypes;
    bool                       m_frozen = false;
};

} // namespace engine
