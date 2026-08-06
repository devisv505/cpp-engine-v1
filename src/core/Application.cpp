#include "core/Application.h"

#include <SDL3/SDL.h>

#include "core/Log.h"
#include "renderer/RendererFactory.h"

namespace engine {

namespace {

// Runtime data sits next to the executable (CMake copies it there), so the app
// finds it no matter which directory it is launched from.
std::string ResolveDataPath(const char* relativePath)
{
    if (const char* basePath = SDL_GetBasePath()) {
        return std::string(basePath) + relativePath;
    }
    return relativePath;
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

    m_config = LoadWindowConfig(ResolveDataPath("config/window.json"));

    m_renderer = CreateRenderer();
    LOG_INFO("Renderer backend: %s", m_renderer->GetBackendName());

    if (!m_window.Init(m_config, GetRequiredWindowFlags())) {
        return false;
    }

    if (!m_renderer->Init(m_window)) {
        LOG_ERROR("Renderer initialization failed");
        return false;
    }

    if (!m_scripts.Init(m_scene, m_window)) {
        return false;
    }

    m_scriptPath = ResolveDataPath("scripts/main.lua");
    RunScript();

    return true;
}

// Rebuilds the scene from the script. A failing script leaves the engine
// running with an empty scene rather than taking it down.
void Application::RunScript()
{
    m_window.GetPixelSize(m_sceneWidth, m_sceneHeight);
    if (!m_scripts.RunFile(m_scriptPath)) {
        LOG_WARN("Scene script failed; rendering an empty scene");
    }
}

void Application::MainLoop()
{
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (event.window.windowID == SDL_GetWindowID(m_window.GetSDLWindow())) {
                    running = false;
                }
                break;

            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                m_renderer->OnResize(event.window.data1, event.window.data2);
                // Scripts lay out against the window size, so re-run on a real
                // size change. SDL also emits this event at window creation,
                // where the scene is already current.
                if (event.window.data1 != m_sceneWidth || event.window.data2 != m_sceneHeight) {
                    RunScript();
                }
                break;

            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_F5 && !event.key.repeat) {
                    LOG_INFO("Reloading %s", m_scriptPath.c_str());
                    RunScript();
                } else if (event.key.key == SDLK_ESCAPE) {
                    running = false;
                }
                break;

            default:
                break;
            }
        }

        RenderFrame();
    }
}

void Application::RenderFrame()
{
    m_renderer->BeginFrame(m_scene.clearColor);
    for (const Quad& quad : m_scene.quads) {
        m_renderer->DrawQuad(quad);
    }
    m_renderer->EndFrame();
}

void Application::Shutdown()
{
    m_scripts.Shutdown();
    if (m_renderer) {
        m_renderer->Shutdown();
        m_renderer.reset();
    }
    m_window.Shutdown();
    SDL_Quit();
}

} // namespace engine
