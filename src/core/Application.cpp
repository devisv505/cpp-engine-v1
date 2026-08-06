#include "core/Application.h"

#include <algorithm>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include "core/Log.h"
#include "renderer/RendererFactory.h"
#include "world/MapPatterns.h"
#include "world/TileAtlas.h"

namespace engine {

namespace {

// Runtime data sits next to the executable (CMake copies it there), so the app
// finds it no matter which directory it is launched from.
std::string BaseDir()
{
    if (const char* basePath = SDL_GetBasePath()) {
        return basePath;
    }
    return "";
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
    m_baseDir = BaseDir();

    m_config = LoadWindowConfig(m_baseDir + "config/window.json");

    m_renderer = CreateRenderer();
    LOG_INFO("Renderer backend: %s", m_renderer->GetBackendName());

    if (!m_window.Init(m_config, GetRequiredWindowFlags())) {
        return false;
    }
    if (!m_renderer->Init(m_window)) {
        LOG_ERROR("Renderer initialization failed");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini clutter next to the binary
    if (!m_renderer->InitImGui(m_window)) {
        return false;
    }
    m_imguiReady = true;

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
    if (!m_scripts.RunFile(m_baseDir + "scripts/main.lua")) {
        LOG_WARN("Scene script failed; continuing with defaults");
    }

    m_registry = TileRegistry();
    m_world    = WorldConfig();
    m_scripts.ReadWorldConfig(m_world, m_registry);
    m_registry.Freeze();

    const TileRenderData renderData = BuildTileRenderData(m_registry, m_baseDir);

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

    m_editor.Init(m_map, m_registry, m_world, m_baseDir);
    m_panUp = m_panDown = m_panLeft = m_panRight = false;
    return true;
}

void Application::HandleEvent(const SDL_Event& event, bool& running)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
    const ImGuiIO& io = ImGui::GetIO();
    EditorCamera&  camera = m_editor.Camera();

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
        if (!io.WantCaptureMouse) {
            camera.OnMouseWheel(event.wheel.y, event.wheel.mouse_x * scale,
                                event.wheel.mouse_y * scale);
        }
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!io.WantCaptureMouse && event.button.button == SDL_BUTTON_MIDDLE) {
            camera.BeginDrag(event.button.x * scale, event.button.y * scale);
        }
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_MIDDLE) {
            camera.EndDrag();
        }
        break;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !io.WantCaptureKeyboard) {
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
        // Releases always clear; presses only register when ImGui does not
        // own the keyboard (typing in a panel must not pan the camera).
        const bool engage = down && !io.WantCaptureKeyboard;
        if (event.key.key == camera.keyUp)    m_panUp    = engage;
        if (event.key.key == camera.keyDown)  m_panDown  = engage;
        if (event.key.key == camera.keyLeft)  m_panLeft  = engage;
        if (event.key.key == camera.keyRight) m_panRight = engage;
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

        const ImGuiIO& io = ImGui::GetIO();
        EditorCamera&  camera = m_editor.Camera();

        float mouseX = 0.0f, mouseY = 0.0f;
        const uint32_t buttons = SDL_GetMouseState(&mouseX, &mouseY);
        int pixelW = 0, pixelH = 0, logicalW = 0, logicalH = 0;
        m_window.GetPixelSize(pixelW, pixelH);
        SDL_GetWindowSize(m_window.GetSDLWindow(), &logicalW, &logicalH);
        const float scale = logicalW > 0 ? static_cast<float>(pixelW) / logicalW : 1.0f;
        const float mousePxX = mouseX * scale;
        const float mousePxY = mouseY * scale;

        camera.SetPanInput(static_cast<float>(m_panRight) - static_cast<float>(m_panLeft),
                           static_cast<float>(m_panDown) - static_cast<float>(m_panUp));
        camera.Update(dt, mousePxX, mousePxY, pixelW, pixelH);

        if (!io.WantCaptureMouse) {
            m_editor.UpdatePainting(mousePxX, mousePxY, buttons);
        }

        int dx = 0, dy = 0, dw = 0, dh = 0;
        if (m_map.TakeDirtyRegion(dx, dy, dw, dh)) {
            m_renderer->UpdateTileRegion(dx, dy, dw, dh, m_map.Data(), m_map.Width());
        }

        RenderFrame();
    }
}

void Application::RenderFrame()
{
    const EditorCamera& camera = m_editor.Camera();

    int pixelW = 0, pixelH = 0;
    m_window.GetPixelSize(pixelW, pixelH);

    TileDrawConstants constants{};
    constants.cameraX    = camera.X();
    constants.cameraY    = camera.Y();
    constants.zoom       = camera.Zoom();
    constants.tileSizePx = MapEditor::kTileSizePx;
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
    for (const Quad& quad : m_scene.quads) {
        m_renderer->DrawQuad(quad);
    }

    m_renderer->BeginImGuiFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const bool mapRecreated = m_editor.BuildUI(camera);
    ImGui::Render();
    m_renderer->RenderImGui();
    m_renderer->EndFrame();

    if (mapRecreated) {
        // New size or freshly loaded contents: rebuild the GPU-side map.
        const TileRenderData renderData = BuildTileRenderData(m_registry, m_baseDir);
        m_renderer->CreateTileResources(renderData, m_map.Width(), m_map.Height(),
                                        m_map.Data());
        int x = 0, y = 0, w = 0, h = 0;
        m_map.TakeDirtyRegion(x, y, w, h);
    }
}

void Application::Shutdown()
{
    if (m_imguiReady) {
        m_renderer->ShutdownImGui();
        ImGui::DestroyContext();
        m_imguiReady = false;
    }
    m_scripts.Shutdown();
    if (m_renderer) {
        m_renderer->Shutdown();
        m_renderer.reset();
    }
    m_window.Shutdown();
    SDL_Quit();
}

} // namespace engine
