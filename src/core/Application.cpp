#include "core/Application.h"

#include <string>

#include <SDL3/SDL.h>

#include "core/Log.h"
#include "renderer/RendererFactory.h"

namespace engine {

namespace {

// The config lives next to the executable (POST_BUILD copy in CMake), so the
// app finds it no matter which directory it is launched from.
std::string ResolveConfigPath()
{
    if (const char* basePath = SDL_GetBasePath()) {
        return std::string(basePath) + "config/window.json";
    }
    return "config/window.json";
}

} // namespace

int Application::Run()
{
    if (!Init()) {
        Shutdown();
        return 1;
    }
    MainLoop();
    Shutdown();
    return 0;
}

bool Application::Init()
{
    SDL_SetAppMetadata("Cpp Engine", "0.1.0", "com.devisv.cppengine");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    m_config = LoadWindowConfig(ResolveConfigPath());

    m_renderer = CreateRenderer();
    LOG_INFO("Renderer backend: %s", m_renderer->GetBackendName());

    if (!m_window.Init(m_config, GetRequiredWindowFlags())) {
        return false;
    }

    if (!m_renderer->Init(m_window)) {
        LOG_ERROR("Renderer initialization failed");
        return false;
    }

    return true;
}

void Application::MainLoop()
{
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                       event.window.windowID == SDL_GetWindowID(m_window.GetSDLWindow())) {
                running = false;
            }
        }

        m_renderer->BeginFrame();
        m_renderer->EndFrame();

        // No swapchain to present against yet; don't busy-spin the CPU.
        SDL_Delay(16);
    }
}

void Application::Shutdown()
{
    if (m_renderer) {
        m_renderer->Shutdown();
        m_renderer.reset();
    }
    m_window.Shutdown();
    SDL_Quit();
}

} // namespace engine
