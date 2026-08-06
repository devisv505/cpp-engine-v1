#pragma once

#include <memory>
#include <string>

#include "core/Config.h"
#include "core/Scene2D.h"
#include "core/Window.h"
#include "renderer/IRenderer.h"
#include "scripting/ScriptHost.h"

namespace engine {

class Application {
public:
    // Runs init -> main loop -> shutdown. Returns the process exit code.
    int Run();

private:
    bool Init();
    void MainLoop();
    void Shutdown();

    void RunScript();
    void RenderFrame();

    WindowConfig               m_config;
    Window                     m_window;
    Scene2D                    m_scene;
    ScriptHost                 m_scripts;
    std::unique_ptr<IRenderer> m_renderer;
    std::string                m_scriptPath;

    // Framebuffer size the current scene was built against.
    int m_sceneWidth  = 0;
    int m_sceneHeight = 0;
};

} // namespace engine
