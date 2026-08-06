#pragma once

#include <cstdint>

#include "core/Scene2D.h"

namespace engine {

class Window;
struct TileRenderData;

// 2D renderer abstraction. Exactly one backend implementation is compiled per
// platform: Direct3D 12 on Windows, Vulkan on Linux, Metal on macOS.
//
// Quads carry no vertex buffers: the vertex shader generates the four corners
// from the vertex index and the per-draw QuadConstants, so a draw is a single
// constant upload plus a 4-vertex triangle strip.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Creates device objects and the swapchain for an already-created window.
    // Returns false on failure; the app aborts startup.
    virtual bool Init(Window& window) = 0;

    // Releases all backend objects. Safe to call more than once.
    virtual void Shutdown() = 0;

    // Acquires the next frame's target and clears it. A frame that fails to
    // acquire is skipped: DrawQuad and EndFrame become no-ops until the next one.
    virtual void BeginFrame(const Color& clearColor) = 0;

    // Draws one quad in pixel coordinates. Only valid between Begin/EndFrame.
    virtual void DrawQuad(const Quad& quad) = 0;

    // Finishes and presents the frame.
    virtual void EndFrame() = 0;

    // Framebuffer size changed (window resize, display scale change).
    virtual void OnResize(int pixelWidth, int pixelHeight) = 0;

    // --- Tile map ---------------------------------------------------------
    // Uploads the atlas + palette (TileRenderData, see world/TileAtlas.h) and
    // the full tile-id grid (R16Uint texture, mapWidth x mapHeight). Callable
    // again with new data (map regenerated or resized): implementations must
    // replace the previous resources after waiting for the GPU.
    virtual bool CreateTileResources(const TileRenderData& data,
                                     int mapWidth, int mapHeight,
                                     const uint16_t* tiles) = 0;

    // Re-uploads the given tile rectangle. `tiles` is the FULL map array
    // (row-major, mapWidth wide); implementations read the region from it.
    virtual void UpdateTileRegion(int x, int y, int w, int h,
                                  const uint16_t* tiles, int mapWidth) = 0;

    // Draws the whole visible map as one fullscreen pass. Only valid between
    // BeginFrame/EndFrame and after CreateTileResources.
    virtual void DrawTileMap(const TileDrawConstants& constants) = 0;

    // --- Dear ImGui -------------------------------------------------------
    // InitImGui runs the SDL3 platform init (ImGui_ImplSDL3_InitFor*) and the
    // graphics-backend init. The application owns the ImGui context and calls
    // ImGui_ImplSDL3_NewFrame/ImGui::NewFrame itself; BeginImGuiFrame is the
    // backend half (Metal needs the frame's render-pass descriptor, so call it
    // between BeginFrame and RenderImGui). RenderImGui draws the current
    // ImGui::GetDrawData into the frame being recorded.
    virtual bool InitImGui(Window& window) = 0;
    virtual void BeginImGuiFrame() = 0;
    virtual void RenderImGui() = 0;
    virtual void ShutdownImGui() = 0;

    virtual const char* GetBackendName() const = 0;
};

} // namespace engine
