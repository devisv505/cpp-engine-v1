#include "core/Application.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>

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

    if (getenv("ENGINE_DRAG_TEST")) {
        int pw = 0, ph = 0; m_window.GetPixelSize(pw, ph);
        const auto toScreen = [&](float wx, float wy, float& sx, float& sy) {
            sx = (wx - m_camera.X()) * m_camera.Zoom() + pw * 0.5f;
            sy = (wy - m_camera.Y()) * m_camera.Zoom() + ph * 0.5f;
        };
        const Light& l0 = m_environment.lights[0];
        const float ox = l0.x, oy = l0.y, odx = l0.dirX, ody = l0.dirY;

        // 1) grab the light body and move it +100,+50 world px
        float sx=0, sy=0; toScreen(ox, oy, sx, sy);
        LOG_INFO("[dragtest] grab body at screen (%.0f,%.0f): %s", sx, sy,
                 BeginLightDrag(sx, sy) ? "HIT" : "MISS");
        UpdateLightDrag(sx + 100.0f * m_camera.Zoom(), sy + 50.0f * m_camera.Zoom());
        LOG_INFO("[dragtest] moved  (%.1f,%.1f) -> (%.1f,%.1f)  expect (%.1f,%.1f)",
                 ox, oy, m_environment.lights[0].x, m_environment.lights[0].y, ox+100.0f, oy+50.0f);
        m_dragTarget = DragTarget::None; m_dragLight = -1;

        // 2) grab the aim handle and point the light straight down
        const Light& l1 = m_environment.lights[0];
        float hx=0, hy=0;
        toScreen(l1.x + l1.dirX * (kDirDistancePx / m_camera.Zoom()),
                 l1.y + l1.dirY * (kDirDistancePx / m_camera.Zoom()), hx, hy);
        LOG_INFO("[dragtest] grab aim at screen (%.0f,%.0f): %s", hx, hy,
                 BeginLightDrag(hx, hy) ? "HIT" : "MISS");
        float bx=0, by=0; toScreen(l1.x, l1.y, bx, by);
        UpdateLightDrag(bx, by + 200.0f);
        LOG_INFO("[dragtest] dir (%.2f,%.2f) -> (%.2f,%.2f)  expect (0.00,1.00)",
                 odx, ody, m_environment.lights[0].dirX, m_environment.lights[0].dirY);
        m_dragTarget = DragTarget::None; m_dragLight = -1;

        // 3) empty space must not grab anything
        LOG_INFO("[dragtest] click empty space: %s (expect MISS)",
                 BeginLightDrag(5.0f, 5.0f) ? "HIT" : "MISS");
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

    // Prime the camera's viewport: events are handled before Update runs, so
    // without this a click on the very first frame would unproject wrongly.
    int pixelWidth = 0, pixelHeight = 0;
    m_window.GetPixelSize(pixelWidth, pixelHeight);
    m_camera.Update(0.0f, 0.0f, 0.0f, pixelWidth, pixelHeight);

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
        } else if (event.button.button == SDL_BUTTON_LEFT) {
            BeginLightDrag(event.button.x * scale, event.button.y * scale);
        }
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_MIDDLE) {
            m_camera.EndDrag();
        } else if (event.button.button == SDL_BUTTON_LEFT) {
            m_dragTarget = DragTarget::None;
            m_dragLight  = -1;
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
        UpdateLightDrag(mouseX * scale, mouseY * scale);

        int dx = 0, dy = 0, dw = 0, dh = 0;
        if (m_map.TakeDirtyRegion(dx, dy, dw, dh)) {
            m_renderer->UpdateTileRegion(dx, dy, dw, dh, m_map.Data(), m_map.Width());
        }

        RenderFrame();
    }
}

bool Application::BeginLightDrag(float mousePxX, float mousePxY)
{
    float worldX = 0.0f, worldY = 0.0f;
    m_camera.ScreenToWorld(mousePxX, mousePxY, worldX, worldY);

    const float zoom       = m_camera.Zoom();
    const float pickRadius = kHandlePx / zoom;      // constant on screen
    const float dirRadius  = kDirHandlePx / zoom;
    const float dirOffset  = kDirDistancePx / zoom;

    // Direction handles sit on top of the bodies, so test them first.
    for (size_t i = 0; i < m_environment.lights.size(); ++i) {
        const Light& light = m_environment.lights[i];
        const float hx = light.x + light.dirX * dirOffset;
        const float hy = light.y + light.dirY * dirOffset;
        const float dx = worldX - hx, dy = worldY - hy;
        if (dx * dx + dy * dy <= dirRadius * dirRadius) {
            m_dragTarget = DragTarget::Direction;
            m_dragLight  = static_cast<int>(i);
            return true;
        }
    }

    for (size_t i = 0; i < m_environment.lights.size(); ++i) {
        const Light& light = m_environment.lights[i];
        const float dx = worldX - light.x, dy = worldY - light.y;
        if (dx * dx + dy * dy <= pickRadius * pickRadius) {
            m_dragTarget  = DragTarget::Position;
            m_dragLight   = static_cast<int>(i);
            m_dragOffsetX = light.x - worldX;
            m_dragOffsetY = light.y - worldY;
            return true;
        }
    }
    return false;
}

void Application::UpdateLightDrag(float mousePxX, float mousePxY)
{
    if (m_dragTarget == DragTarget::None || m_dragLight < 0 ||
        m_dragLight >= static_cast<int>(m_environment.lights.size())) {
        return;
    }

    float worldX = 0.0f, worldY = 0.0f;
    m_camera.ScreenToWorld(mousePxX, mousePxY, worldX, worldY);
    Light& light = m_environment.lights[m_dragLight];

    if (m_dragTarget == DragTarget::Position) {
        light.x = worldX + m_dragOffsetX;
        light.y = worldY + m_dragOffsetY;
        return;
    }

    const float dx = worldX - light.x, dy = worldY - light.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length > 1e-4f) {
        light.dirX = dx / length;
        light.dirY = dy / length;
    }
}

void Application::DrawLightHandles()
{
    const float zoom = m_camera.Zoom();
    int pixelW = 0, pixelH = 0;
    m_window.GetPixelSize(pixelW, pixelH);

    // World -> screen, matching the tile and light shaders.
    const auto toScreen = [&](float wx, float wy, float& sx, float& sy) {
        sx = (wx - m_camera.X()) * zoom + pixelW * 0.5f;
        sy = (wy - m_camera.Y()) * zoom + pixelH * 0.5f;
    };

    for (size_t i = 0; i < m_environment.lights.size(); ++i) {
        const Light& light  = m_environment.lights[i];
        const bool   active = static_cast<int>(i) == m_dragLight &&
                              m_dragTarget != DragTarget::None;

        float bodyX = 0.0f, bodyY = 0.0f;
        toScreen(light.x, light.y, bodyX, bodyY);

        // Quads do not blend, so the border is drawn first and the coloured
        // body over it, leaving a visible frame. It widens while dragging.
        Quad border;
        border.w = border.h = kHandlePx + (active ? 10.0f : 5.0f);
        border.x = bodyX - border.w * 0.5f;
        border.y = bodyY - border.h * 0.5f;
        border.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
        m_renderer->DrawQuad(border);

        Quad body;
        body.w = body.h = kHandlePx;
        body.x = bodyX - kHandlePx * 0.5f;
        body.y = bodyY - kHandlePx * 0.5f;
        body.color = light.color;
        body.color.a = 1.0f;
        m_renderer->DrawQuad(body);

        float dirX = 0.0f, dirY = 0.0f;
        toScreen(light.x + light.dirX * (kDirDistancePx / zoom),
                 light.y + light.dirY * (kDirDistancePx / zoom), dirX, dirY);

        Quad aim;
        aim.w = aim.h = kDirHandlePx;
        aim.x = dirX - kDirHandlePx * 0.5f;
        aim.y = dirY - kDirHandlePx * 0.5f;
        aim.color = Color{1.0f, 1.0f, 1.0f, 0.85f};
        m_renderer->DrawQuad(aim);
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
    // Volumetric scattering belongs behind opaque scene geometry. Drawing it
    // after walls exposed the ray march inside each blocker as a noisy,
    // translucent band along the lit edge.
    m_renderer->DrawLighting(m_environment.lights.data(),
                             static_cast<int>(m_environment.lights.size()),
                             constants);
    m_renderer->DrawWalls(m_environment.walls.data(),
                          static_cast<int>(m_environment.walls.size()));
    DrawLightHandles();
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
