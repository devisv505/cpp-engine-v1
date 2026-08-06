#pragma once

#include <string>

#include "editor/EditorCamera.h"
#include "world/TileMap.h"
#include "world/TileRegistry.h"
#include "world/WorldConfig.h"

namespace engine {

// The Map Editor: tile painting, brush control, new/save/load, and the ImGui
// panel. Gameplay and the editor share TileMap and the tile renderer — the
// editor is just another writer of the same data.
class MapEditor {
public:
    static constexpr float kTileSizePx = 32.0f;

    void Init(TileMap& map, TileRegistry& registry, WorldConfig& config,
              const std::string& baseDir);

    // Mouse painting; call only when ImGui does not want the mouse.
    // Buttons: SDL button state bitmask.
    void UpdatePainting(float mouseX, float mouseY, uint32_t mouseButtons);

    // Builds the ImGui panel. Returns true if the map was recreated (new size
    // or load) so the caller can rebuild GPU tile resources.
    bool BuildUI(const EditorCamera& camera);

    EditorCamera& Camera() { return m_camera; }

    TileId SelectedTile() const { return m_selectedTile; }
    int    BrushSize() const { return m_brushSize; }

private:
    bool NewMap();
    bool Save();
    bool Load();

    TileMap*      m_map      = nullptr;
    TileRegistry* m_registry = nullptr;
    WorldConfig*  m_config   = nullptr;

    EditorCamera m_camera;

    TileId m_selectedTile = 1;
    int    m_brushSize    = 1;

    int  m_newWidth  = 256;
    int  m_newHeight = 256;
    int  m_patternIndex = 0;
    char m_fileName[128] = "map.cmap";

    std::string m_mapsDir;
    std::string m_status;
};

} // namespace engine
