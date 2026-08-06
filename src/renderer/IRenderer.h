#pragma once

#include <cstdint>

#include "core/Scene2D.h"

namespace engine {

class Window;
struct TileRenderData;
struct Wall;
struct Light;

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

    // --- Volumetric lighting ---------------------------------------------
    // Occluders are cached: this rebuilds the world-space occlusion mask that
    // the light shaders raymarch against, and is called only when the wall set
    // changes (not per frame). `originX/Y` and `worldWidth/Height` describe the
    // world-pixel rectangle the mask covers.
    virtual void SetOccluders(const Wall* walls, int wallCount,
                              float originX, float originY,
                              float worldWidth, float worldHeight) = 0;

    // Draws walls as world-space quads, transformed by the camera constants
    // already supplied through DrawTileMap. Valid between Begin/EndFrame.
    virtual void DrawWalls(const Wall* walls, int wallCount) = 0;

    // Accumulates every visible light over the scene. Backends cull lights
    // against the viewport, run the cone raymarch and the god-ray pass, then
    // blend the result additively. Valid between Begin/EndFrame.
    virtual void DrawLighting(const Light* lights, int lightCount,
                              const TileDrawConstants& camera) = 0;

    virtual const char* GetBackendName() const = 0;
};

} // namespace engine
