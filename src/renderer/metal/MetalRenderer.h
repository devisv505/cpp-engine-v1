#pragma once

#include "renderer/IRenderer.h"

namespace engine {

// Objective-C objects live behind this pimpl (defined in MetalRenderer.mm),
// keeping this header includable from plain C++.
struct MetalState;

class MetalRenderer final : public IRenderer {
public:
    ~MetalRenderer() override;

    bool Init(Window& window) override;
    void Shutdown() override;
    void BeginFrame(const Color& clearColor) override;
    void DrawQuad(const Quad& quad) override;
    void EndFrame() override;
    void OnResize(int pixelWidth, int pixelHeight) override;
    bool CreateTileResources(const TileRenderData& data,
                             int mapWidth, int mapHeight,
                             const uint16_t* tiles) override;
    void UpdateTileRegion(int x, int y, int w, int h,
                          const uint16_t* tiles, int mapWidth) override;
    void DrawTileMap(const TileDrawConstants& constants) override;
    void SetOccluders(const Wall* walls, int wallCount,
                      float originX, float originY,
                      float worldWidth, float worldHeight) override;
    void DrawWalls(const Wall* walls, int wallCount) override;
    void DrawLighting(const Light* lights, int lightCount,
                      const TileDrawConstants& camera) override;
    [[nodiscard]] const char* GetBackendName() const override { return "Metal"; }

private:
    bool CreateLightingPipelines();

    MetalState* m_state = nullptr;
};

} // namespace engine
