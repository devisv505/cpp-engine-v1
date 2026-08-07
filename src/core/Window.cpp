#include "core/Window.h"

#include <SDL3/SDL_error.h>

#include "core/Log.h"

namespace engine {

Window::~Window()
{
    Shutdown();
}

bool Window::Init(const std::string& configPath, SDL_WindowFlags backendFlags)
{
    m_config = LoadWindowConfig(configPath);

    SDL_WindowFlags flags = backendFlags;
    if (m_config.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    if (m_config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    m_window = SDL_CreateWindow(m_config.title.c_str(), m_config.width, m_config.height, flags);
    if (!m_window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    LOG_INFO("Window created: \"%s\" %dx%d%s%s",
             m_config.title.c_str(), m_config.width, m_config.height,
             m_config.fullscreen ? " fullscreen" : "",
             m_config.resizable ? " resizable" : "");
    return true;
}

void Window::GetPixelSize(int& width, int& height) const
{
    width  = 0;
    height = 0;
    if (m_window) {
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
    }
}

float Window::GetPixelDensity() const
{
    if (!m_window) {
        return 1.0f;
    }
    int pixelW = 0, pixelH = 0, logicalW = 0, logicalH = 0;
    SDL_GetWindowSizeInPixels(m_window, &pixelW, &pixelH);
    SDL_GetWindowSize(m_window, &logicalW, &logicalH);
    return logicalW > 0 ? static_cast<float>(pixelW) / static_cast<float>(logicalW) : 1.0f;
}

void Window::Shutdown()
{
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

} // namespace engine
