#pragma once

#include <memory>
#include <string>

#include "core/Config.h"
#include "core/Scene2D.h"
#include "core/Window.h"
#include "editor/EditorCamera.h"
#include "renderer/IRenderer.h"
#include "scripting/ScriptHost.h"
#include "world/Environment.h"
#include "world/TileMap.h"
#include "world/TileRegistry.h"
#include "world/WorldConfig.h"

union SDL_Event;

namespace engine {

class Application {
public:
    // World pixels per tile. The camera and the tile shader share this scale.
    static constexpr float kTileSizePx = 32.0f;

    // Runs init -> main loop -> shutdown. Returns the process exit code.
    int Run();

private:
    bool Init();
    void MainLoop();
    void Shutdown();

    // Runs the script and rebuilds the world from its globals: tile registry,
    // atlas, generated map, GPU tile resources, camera.
    bool RebuildWorld();
    void HandleEvent(const SDL_Event& event, bool& running);
    void RenderFrame();

    // --- Light gizmos -----------------------------------------------------
    // Lights can be dragged at runtime. Edits are deliberately ephemeral: F5
    // or a restart re-reads the script and discards them.
    enum class DragTarget { None, Position, Direction };

    // Grabs the light under the cursor, if any. Returns true when a drag began.
    bool BeginLightDrag(float mousePxX, float mousePxY);
    void UpdateLightDrag(float mousePxX, float mousePxY);
    void DrawLightHandles();

    // Screen-space sizes, so handles stay grabbable at any zoom.
    static constexpr float kHandlePx     = 14.0f;  // light body handle
    static constexpr float kDirHandlePx  = 10.0f;  // direction handle
    static constexpr float kDirDistancePx = 72.0f; // how far the aim handle sits

    WindowConfig               m_config;
    Window                     m_window;
    Scene2D                    m_scene;
    ScriptHost                 m_scripts;
    std::unique_ptr<IRenderer> m_renderer;

    TileRegistry m_registry;
    TileMap      m_map;
    WorldConfig  m_world;
    Environment  m_environment;
    EditorCamera m_camera;

    uint64_t m_lastFrameNs = 0;

    // Keyboard pan state (configurable keys, see EditorConfig).
    bool m_panUp = false, m_panDown = false, m_panLeft = false, m_panRight = false;

    DragTarget m_dragTarget = DragTarget::None;
    int        m_dragLight  = -1;
    // Offset from the light's origin to the grab point, so a light does not
    // snap its centre to the cursor when picked up.
    float m_dragOffsetX = 0.0f, m_dragOffsetY = 0.0f;
};

} // namespace engine
