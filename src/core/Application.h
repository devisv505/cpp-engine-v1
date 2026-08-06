#pragma once

#include <memory>

#include "core/Config.h"
#include "core/Window.h"
#include "renderer/IRenderer.h"

namespace engine {

class Application {
public:
    // Runs init -> main loop -> shutdown. Returns the process exit code.
    int Run();

private:
    bool Init();
    void MainLoop();
    void Shutdown();

    WindowConfig               m_config;
    Window                     m_window;
    std::unique_ptr<IRenderer> m_renderer;
};

} // namespace engine
