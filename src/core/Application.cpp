#include "core/Application.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <SDL3/SDL.h>

#include "core/Log.h"
#include "renderer/RendererFactory.h"
#include "world/MapPatterns.h"
#include "world/TileAtlas.h"

namespace engine {

namespace {

// Three columns by five rows, row-major. Only the glyphs used by the FPS HUD
// are included so the overlay stays independent of a font asset or text API.
const char* FpsGlyph(char character)
{
    switch (character) {
    case 'F': return "111100110100100";
    case 'P': return "110101110100100";
    case 'S': return "111100111001111";
    case '0': return "111101101101111";
    case '1': return "010110010010111";
    case '2': return "111001111100111";
    case '3': return "111001111001111";
    case '4': return "101101111001001";
    case '5': return "111100111001111";
    case '6': return "111100111101111";
    case '7': return "111001001001001";
    case '8': return "111101111101111";
    case '9': return "111101111001111";
    case ' ': return "000000000000000";
    default:  return "000000000000000";
    }
}

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

    // Subscribers first, pump later: everything must be listening before the
    // first Pump() in MainLoop publishes anything.
    m_input.Init(m_events, m_baseDir + "config/input.json");
    m_onQuit = m_events.Subscribe<QuitRequested>([this](const QuitRequested&) {
        m_running = false;
    });
    m_onWorldRebuilt = m_events.Subscribe<WorldRebuilt>([](const WorldRebuilt& e) {
        LOG_INFO("World rebuilt: %dx%d tiles", e.mapSize.x, e.mapSize.y);
    });

    m_renderer = CreateRenderer();
    LOG_INFO("Renderer backend: %s", m_renderer->GetBackendName());

    if (!m_window.Init(m_baseDir + "config/window.json", GetRequiredWindowFlags())) {
        return false;
    }
    if (!m_renderer->Init(m_window, m_events)) {
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
    if (!m_scripts.RunFile(m_baseDir + "scripts/main.lua")) {
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

    m_camera.Configure(m_world.editor,
                       static_cast<int>(m_map.Width() * kTileSizePx),
                       static_cast<int>(m_map.Height() * kTileSizePx));

    m_events.Emit(WorldRebuilt{{m_map.Width(), m_map.Height()}});
    m_input.Clear();  // a rebuild should not inherit keys held during the old world
    return true;
}

void Application::Update(const float deltaTime)
{
    // One-shot actions: the press edge, so holding the key does not repeat.
    if (m_input.WasActionPressed(Action::Quit)) {
        m_events.Emit(QuitRequested{});
        return;
    }
    if (m_input.WasActionPressed(Action::ReloadScript)) {
        LOG_INFO("Reloading script and regenerating the world");
        RebuildWorld();
        return;
    }

    m_cameraController.Update(deltaTime);
}

void Application::MainLoop()
{
    m_running = true;
    while (m_running) {
        // Advances the input state when it goes out of scope, whatever path
        // this iteration takes out of the loop body.
        const InputFrame inputFrame(m_input);

        // Publishes this frame's window and input events; subscribers react
        // synchronously (InputState records it, the renderer resizes, m_running
        // drops on QuitRequested).
        m_eventPump.Pump();

        const uint64_t nowNs = SDL_GetTicksNS();
        const float dt = std::min(0.1f, static_cast<float>(nowNs - m_lastFrameNs) * 1e-9f);
        m_lastFrameNs  = nowNs;
        UpdateFps(dt);

        Update(dt);

        int dx = 0, dy = 0, dw = 0, dh = 0;
        if (m_map.TakeDirtyRegion(dx, dy, dw, dh)) {
            m_renderer->UpdateTileRegion(dx, dy, dw, dh, m_map.Data(), m_map.Width());
        }

        RenderFrame();
    }
}

void Application::UpdateFps(float frameSeconds)
{
    if (!(frameSeconds > 0.0f)) {
        return;
    }

    m_fpsElapsed += frameSeconds;
    ++m_fpsFrames;

    // Half a second reacts quickly to real performance changes without making
    // the displayed integer flicker from one unusually long frame.
    if (m_fpsElapsed >= 0.5f) {
        m_displayFps = static_cast<uint32_t>(
            std::lround(static_cast<float>(m_fpsFrames) / m_fpsElapsed));
        m_fpsElapsed = 0.0f;
        m_fpsFrames  = 0;
    }
}

void Application::DrawFpsOverlay()
{
    const float density = std::max(1.0f, m_window.GetPixelDensity());
    const float cell    = std::max(2.0f, std::round(3.0f * density));
    const float padding  = cell * 1.5f;
    const float originX  = cell * 2.0f;
    const float originY  = cell * 2.0f;
    const std::string label =
        "FPS " + std::to_string(std::min<uint32_t>(m_displayFps, 9999));

    Quad panel;
    panel.x = originX - padding;
    panel.y = originY - padding;
    panel.w = (static_cast<float>(label.size() * 4 - 1) * cell) + padding * 2.0f;
    panel.h = 5.0f * cell + padding * 2.0f;
    panel.color = Color{0.025f, 0.03f, 0.04f, 1.0f};
    m_renderer->DrawQuad(panel);

    const Color textColor{0.72f, 1.0f, 0.76f, 1.0f};
    for (size_t glyphIndex = 0; glyphIndex < label.size(); ++glyphIndex) {
        const char* glyph = FpsGlyph(label[glyphIndex]);
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (glyph[row * 3 + column] != '1') {
                    continue;
                }
                Quad pixel;
                pixel.x = originX + static_cast<float>(glyphIndex * 4 + column) * cell;
                pixel.y = originY + static_cast<float>(row) * cell;
                pixel.w = cell;
                pixel.h = cell;
                pixel.color = textColor;
                m_renderer->DrawQuad(pixel);
            }
        }
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
    for (const Quad& quad : m_scene.quads) {
        m_renderer->DrawQuad(quad);
    }
    DrawFpsOverlay();
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
