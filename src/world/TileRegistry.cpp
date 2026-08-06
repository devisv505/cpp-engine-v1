#include "world/TileRegistry.h"

#include "core/Log.h"

namespace engine {

TileRegistry::TileRegistry()
{
    TilePrototype voidTile;
    voidTile.name  = "void";
    voidTile.color = Color{0.0f, 0.0f, 0.0f, 1.0f};
    m_prototypes.push_back(std::move(voidTile));
}

TileId TileRegistry::Add(TilePrototype prototype)
{
    if (m_frozen) {
        LOG_ERROR("TileRegistry: cannot add '%s' after freeze", prototype.name.c_str());
        return 0;
    }
    if (m_prototypes.size() >= kMaxTileTypes) {
        LOG_ERROR("TileRegistry: limit of %u tile types reached, '%s' ignored",
                  kMaxTileTypes, prototype.name.c_str());
        return 0;
    }
    if (FindByName(prototype.name) != 0 || prototype.name == "void") {
        LOG_ERROR("TileRegistry: duplicate tile name '%s'", prototype.name.c_str());
        return 0;
    }

    m_prototypes.push_back(std::move(prototype));
    return static_cast<TileId>(m_prototypes.size() - 1);
}

TileId TileRegistry::FindByName(const std::string& name) const
{
    for (size_t i = 0; i < m_prototypes.size(); ++i) {
        if (m_prototypes[i].name == name) {
            return static_cast<TileId>(i);
        }
    }
    return 0;
}

const TilePrototype& TileRegistry::Get(TileId id) const
{
    return m_prototypes[id < m_prototypes.size() ? id : 0];
}

TilePrototype& TileRegistry::GetMutable(TileId id)
{
    return m_prototypes[id < m_prototypes.size() ? id : 0];
}

} // namespace engine
