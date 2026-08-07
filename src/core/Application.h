#pragma once

#include <memory>
#include <string>

#include "core/Config.h"
#include "core/input/Input.h"
#include "core/input/InputMap.h"
#include "core/Scene2D.h"
#include "core/Window.h"
#include "editor/EditorCamera.h"
#include "renderer/IRenderer.h"
#include "scripting/ScriptHost.h"
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

    // Per-frame input-driven logic: camera movement, zoom, drag, one-shot
    // actions. Everything here polls Input rather than reacting to events.
    // Returns false when an action asked the engine to quit.
    [[nodiscard]] bool Update(float deltaTime);
    void UpdateFps(float frameSeconds);
    void DrawFpsOverlay();
    void RenderFrame();

    WindowConfig               m_config;
    Window                     m_window;
    Scene2D                    m_scene;
    ScriptHost                 m_scripts;
    std::unique_ptr<IRenderer> m_renderer;
    std::string                m_baseDir;

    TileRegistry m_registry;
    TileMap      m_map;
    WorldConfig  m_world;
    EditorCamera m_camera;
    Input        m_input;
    InputMap     m_inputMap;

    uint64_t m_lastFrameNs = 0;
    float    m_fpsElapsed  = 0.0f;
    uint32_t m_fpsFrames   = 0;
    uint32_t m_displayFps  = 0;
};

} // namespace engine
