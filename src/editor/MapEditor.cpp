#include "editor/MapEditor.h"

#include <algorithm>
#include <filesystem>

#include <SDL3/SDL_mouse.h>
#include <imgui.h>

#include "core/Log.h"
#include "world/MapIO.h"
#include "world/MapPatterns.h"

namespace engine {

void MapEditor::Init(TileMap& map, TileRegistry& registry, WorldConfig& config,
                     const std::string& baseDir)
{
    m_map      = &map;
    m_registry = &registry;
    m_config   = &config;
    m_mapsDir  = baseDir + "maps/";

    m_newWidth  = map.Width();
    m_newHeight = map.Height();

    const auto& names = KnownPatternNames();
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == config.pattern) {
            m_patternIndex = static_cast<int>(i);
        }
    }

    // Default selection: first real tile type.
    m_selectedTile = registry.Count() > 1 ? 1 : 0;

    m_camera.Configure(config.editor,
                       static_cast<int>(map.Width() * kTileSizePx),
                       static_cast<int>(map.Height() * kTileSizePx));
}

void MapEditor::UpdatePainting(float mouseX, float mouseY, uint32_t mouseButtons)
{
    float worldX = 0.0f, worldY = 0.0f;
    m_camera.ScreenToWorld(mouseX, mouseY, worldX, worldY);
    const int tileX = static_cast<int>(std::floor(worldX / kTileSizePx));
    const int tileY = static_cast<int>(std::floor(worldY / kTileSizePx));

    if (mouseButtons & SDL_BUTTON_LMASK) {
        m_map->PaintBrush(tileX, tileY, m_brushSize, m_selectedTile);
    } else if (mouseButtons & SDL_BUTTON_RMASK) {
        // Eyedropper: pick the tile type under the cursor.
        if (m_map->InBounds(tileX, tileY)) {
            m_selectedTile = m_map->Get(tileX, tileY);
        }
    }
}

bool MapEditor::BuildUI(const EditorCamera& camera)
{
    bool mapRecreated = false;

    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Map Editor");

    // --- Tile palette ---
    ImGui::SeparatorText("Tiles");
    for (TileId id = 1; id < m_registry->Count(); ++id) {
        const TilePrototype& tile = m_registry->Get(id);
        ImGui::PushID(id);

        const ImVec4 swatch(tile.color.r, tile.color.g, tile.color.b, 1.0f);
        if (ImGui::ColorButton("##swatch", swatch,
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker)) {
            m_selectedTile = id;
        }
        ImGui::SameLine();
        const std::string label = tile.name + (tile.hasTexture ? " [tex]" : "");
        if (ImGui::Selectable(label.c_str(), m_selectedTile == id)) {
            m_selectedTile = id;
        }
        ImGui::PopID();
    }

    ImGui::SeparatorText("Brush");
    ImGui::SliderInt("Size", &m_brushSize, 1, 32);

    // --- New map ---
    ImGui::SeparatorText("New map");
    const auto& patterns = KnownPatternNames();
    if (ImGui::BeginCombo("Pattern", patterns[m_patternIndex].c_str())) {
        for (size_t i = 0; i < patterns.size(); ++i) {
            if (ImGui::Selectable(patterns[i].c_str(), m_patternIndex == static_cast<int>(i))) {
                m_patternIndex = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::InputInt("Width", &m_newWidth);
    ImGui::InputInt("Height", &m_newHeight);
    if (ImGui::Button("Generate new map")) {
        mapRecreated = NewMap();
    }

    // --- Save / load ---
    ImGui::SeparatorText("File");
    ImGui::InputText("##filename", m_fileName, sizeof(m_fileName));
    if (ImGui::Button("Save")) {
        Save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        mapRecreated = Load();
    }

    if (!m_status.empty()) {
        ImGui::TextWrapped("%s", m_status.c_str());
    }

    ImGui::SeparatorText("View");
    ImGui::Text("Map %dx%d  zoom %.2f", m_map->Width(), m_map->Height(), camera.Zoom());
    ImGui::TextDisabled("LMB paint, RMB pick, MMB drag, wheel zoom");

    ImGui::End();
    return mapRecreated;
}

bool MapEditor::NewMap()
{
    const int width  = std::clamp(m_newWidth, 1, TileMap::kMaxDimension);
    const int height = std::clamp(m_newHeight, 1, TileMap::kMaxDimension);
    if (!m_map->Create(width, height)) {
        m_status = "Failed to create map";
        return false;
    }
    GenerateMapPattern(*m_map, KnownPatternNames()[m_patternIndex], m_config->params);
    m_camera.Configure(m_config->editor,
                       static_cast<int>(width * kTileSizePx),
                       static_cast<int>(height * kTileSizePx));
    m_status = "New map generated";
    return true;
}

bool MapEditor::Save()
{
    std::error_code ec;
    std::filesystem::create_directories(m_mapsDir, ec);
    const std::string path = m_mapsDir + m_fileName;
    const bool ok = SaveMap(*m_map, *m_registry, path);
    m_status = ok ? ("Saved " + std::string(m_fileName)) : ("Save failed: " + path);
    return ok;
}

bool MapEditor::Load()
{
    const std::string path = m_mapsDir + m_fileName;
    if (!LoadMap(*m_map, *m_registry, path)) {
        m_status = "Load failed: " + path;
        return false;
    }
    m_camera.Configure(m_config->editor,
                       static_cast<int>(m_map->Width() * kTileSizePx),
                       static_cast<int>(m_map->Height() * kTileSizePx));
    m_status = "Loaded " + std::string(m_fileName);
    return true;
}

} // namespace engine
