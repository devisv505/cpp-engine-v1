#pragma once

#include <SDL3/SDL_video.h>

#include "core/Config.h"

namespace engine {

// Thin RAII wrapper around SDL_Window.
class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // backendFlags: creation flags required by the active renderer backend,
    // obtained from GetRequiredWindowFlags() before any renderer exists.
    bool Init(const WindowConfig& config, SDL_WindowFlags backendFlags);
    void Shutdown();

    SDL_Window* GetSDLWindow() const { return m_window; }
    const WindowConfig& GetConfig() const { return m_config; }

private:
    SDL_Window*  m_window = nullptr;
    WindowConfig m_config;
};

} // namespace engine
