#pragma once

#include <string>

#include <SDL3/SDL_video.h>

#include "core/Config.h"

namespace engine {

// Owns the SDL window and its configuration. RAII: destroying the Window
// destroys the SDL window.
class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Loads window settings from the JSON file at configPath (falling back to
    // defaults if it is missing or invalid) and creates the window.
    // backendFlags: creation flags required by the active renderer backend,
    // obtained from GetRequiredWindowFlags() before any renderer exists.
    bool Init(const std::string& configPath, SDL_WindowFlags backendFlags);
    void Shutdown();

    [[nodiscard]] SDL_Window* GetSDLWindow() const { return m_window; }
    [[nodiscard]] const WindowConfig& GetConfig() const { return m_config; }

    // Framebuffer size in pixels, which differs from the logical window size on
    // HiDPI displays. This is what the renderer's viewport must use.
    void GetPixelSize(int& width, int& height) const;

    // Framebuffer pixels per window point (1.0 on standard displays, 2.0 on
    // typical HiDPI). Input events report points; multiply by this for pixels.
    [[nodiscard]] float GetPixelDensity() const;

private:
    SDL_Window*  m_window = nullptr;
    WindowConfig m_config;
};

} // namespace engine
