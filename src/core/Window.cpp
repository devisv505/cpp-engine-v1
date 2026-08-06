#include "core/Window.h"

#include <SDL3/SDL_error.h>

#include "core/Log.h"

namespace engine {

Window::~Window()
{
    Shutdown();
}

bool Window::Init(const WindowConfig& config, SDL_WindowFlags backendFlags)
{
    m_config = config;

    SDL_WindowFlags flags = backendFlags;
    if (config.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    if (config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    m_window = SDL_CreateWindow(config.title.c_str(), config.width, config.height, flags);
    if (!m_window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    LOG_INFO("Window created: \"%s\" %dx%d%s%s",
             config.title.c_str(), config.width, config.height,
             config.fullscreen ? " fullscreen" : "",
             config.resizable ? " resizable" : "");
    return true;
}

void Window::Shutdown()
{
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

} // namespace engine
