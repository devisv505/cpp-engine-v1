#include "core/Application.h"

#include <algorithm>

#include <SDL3/SDL.h>

#include "core/Log.h"
#include "core/Paths.h"
#include "renderer/RendererFactory.h"
#include "world/MapPatterns.h"
#include "world/TileAtlas.h"

namespace engine {

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
    if (!RebuildWorld()) {
        return false;
    }

    m_lastFrameNs = SDL_GetTicksNS();
    return true;
}

bool Application::RebuildWorld()
{
    if (!m_scripts.RunFile(ResolveDataPath("scripts/main.lua"))) {
        LOG_WARN("Scene script failed; continuing with defaults");
    }

    m_registry = TileRegistry();
    m_world    = WorldConfig();
    m_scripts.ReadWorldConfig(m_world, m_registry);

    // A script that defines no usable tiles would generate an all-void (black)
    // map, which reads as a broken engine. Fall back to the default two-tone
    // checkerboard so the view always starts on something meaningful.
    if (m_world.params.tiles.empty()) {
        LOG_WARN("Script defined no map tiles; falling back to the default checkerboard");
        TilePrototype dark;
        dark.name  = "default_dark";
        dark.color = Color{0.16f, 0.16f, 0.16f, 1.0f};
        TilePrototype light;
        light.name  = "default_light";
        light.color = Color{0.25f, 0.25f, 0.25f, 1.0f};

        const TileId darkId  = m_registry.Add(std::move(dark));
        const TileId lightId = m_registry.Add(std::move(light));
        if (darkId != 0 && lightId != 0) {
            m_world.params.tiles = {darkId, lightId};
            m_world.pattern      = "checkerboard";
        }
    }

    m_registry.Freeze();

    const TileRenderData renderData = BuildTileRenderData(m_registry);

    if (!m_map.Create(m_world.width, m_world.height)) {
        return false;
    }
    GenerateMapPattern(m_map, m_world.pattern, m_world.params);

    if (!m_renderer->CreateTileResources(renderData, m_map.Width(), m_map.Height(),
                                         m_map.Data())) {
        return false;
    }
    int x = 0, y = 0, w = 0, h = 0;
    m_map.TakeDirtyRegion(x, y, w, h);  // CreateTileResources uploaded everything

    m_environment.Clear();
    m_scripts.ReadEnvironment(m_environment, kTileSizePx);

    // The occlusion mask is cached: rebuilt here when walls change, never per
    // frame. It covers the whole map in world pixels.
    m_renderer->SetOccluders(m_environment.walls.data(),
                             static_cast<int>(m_environment.walls.size()),
                             0.0f, 0.0f,
                             m_map.Width() * kTileSizePx,
                             m_map.Height() * kTileSizePx);

    m_camera.Configure(m_world.editor,
                       static_cast<int>(m_map.Width() * kTileSizePx),
                       static_cast<int>(m_map.Height() * kTileSizePx));
    m_panUp = m_panDown = m_panLeft = m_panRight = false;
    return true;
}

void Application::HandleEvent(const SDL_Event& event, bool& running)
{
    // Mouse coordinates arrive in window points; the camera works in
    // framebuffer pixels, so scale by the pixel density.
    int pixelW = 0, pixelH = 0, logicalW = 0, logicalH = 0;
    m_window.GetPixelSize(pixelW, pixelH);
    SDL_GetWindowSize(m_window.GetSDLWindow(), &logicalW, &logicalH);
    const float scale = logicalW > 0 ? static_cast<float>(pixelW) / logicalW : 1.0f;

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
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        m_camera.OnMouseWheel(event.wheel.y, event.wheel.mouse_x * scale,
                              event.wheel.mouse_y * scale);
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_MIDDLE) {
            m_camera.BeginDrag(event.button.x * scale, event.button.y * scale);
        }
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_MIDDLE) {
            m_camera.EndDrag();
        }
        break;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down) {
            if (event.key.key == SDLK_F5 && !event.key.repeat) {
                LOG_INFO("Reloading script and regenerating the world");
                RebuildWorld();
                return;
            }
            if (event.key.key == SDLK_ESCAPE) {
                running = false;
                return;
            }
        }
        if (event.key.key == m_camera.keyUp)    m_panUp    = down;
        if (event.key.key == m_camera.keyDown)  m_panDown  = down;
        if (event.key.key == m_camera.keyLeft)  m_panLeft  = down;
        if (event.key.key == m_camera.keyRight) m_panRight = down;
        break;
    }

    default:
        break;
    }
}

void Application::MainLoop()
{
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            HandleEvent(event, running);
        }

        const uint64_t nowNs = SDL_GetTicksNS();
        const float dt = std::min(0.1f, static_cast<float>(nowNs - m_lastFrameNs) * 1e-9f);
        m_lastFrameNs  = nowNs;

        float mouseX = 0.0f, mouseY = 0.0f;
        SDL_GetMouseState(&mouseX, &mouseY);
        int pixelW = 0, pixelH = 0, logicalW = 0, logicalH = 0;
        m_window.GetPixelSize(pixelW, pixelH);
        SDL_GetWindowSize(m_window.GetSDLWindow(), &logicalW, &logicalH);
        const float scale = logicalW > 0 ? static_cast<float>(pixelW) / logicalW : 1.0f;

        m_camera.SetPanInput(static_cast<float>(m_panRight) - static_cast<float>(m_panLeft),
                             static_cast<float>(m_panDown) - static_cast<float>(m_panUp));
        m_camera.Update(dt, mouseX * scale, mouseY * scale, pixelW, pixelH);

        int dx = 0, dy = 0, dw = 0, dh = 0;
        if (m_map.TakeDirtyRegion(dx, dy, dw, dh)) {
            m_renderer->UpdateTileRegion(dx, dy, dw, dh, m_map.Data(), m_map.Width());
        }

        RenderFrame();
    }
}

void Application::RenderFrame()
{
    int pixelW = 0, pixelH = 0;
    m_window.GetPixelSize(pixelW, pixelH);

    TileDrawConstants constants{};
    constants.cameraX    = m_camera.X();
    constants.cameraY    = m_camera.Y();
    constants.zoom       = m_camera.Zoom();
    constants.tileSizePx = kTileSizePx;
    constants.viewportW  = static_cast<float>(pixelW);
    constants.viewportH  = static_cast<float>(pixelH);
    constants.mapWidth   = static_cast<float>(m_map.Width());
    constants.mapHeight  = static_cast<float>(m_map.Height());
    constants.background[0] = m_world.background.r;
    constants.background[1] = m_world.background.g;
    constants.background[2] = m_world.background.b;
    constants.background[3] = m_world.background.a;

    m_renderer->BeginFrame(m_scene.clearColor);
    m_renderer->DrawTileMap(constants);
    m_renderer->DrawWalls(m_environment.walls.data(),
                          static_cast<int>(m_environment.walls.size()));
    m_renderer->DrawLighting(m_environment.lights.data(),
                             static_cast<int>(m_environment.lights.size()),
                             constants);
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
