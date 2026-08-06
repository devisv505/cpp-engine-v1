#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "renderer/IRenderer.h"

namespace engine {

class VulkanRenderer final : public IRenderer {
public:
    ~VulkanRenderer() override;

    bool Init(Window& window) override;
    void Shutdown() override;
    void BeginFrame(const Color& clearColor) override;
    void DrawQuad(const Quad& quad) override;
    void EndFrame() override;
    void OnResize(int pixelWidth, int pixelHeight) override;

    bool CreateTileResources(const TileRenderData& data, int mapWidth, int mapHeight,
                             const uint16_t* tiles) override;
    void UpdateTileRegion(int x, int y, int w, int h, const uint16_t* tiles,
                          int mapWidth) override;
    void DrawTileMap(const TileDrawConstants& constants) override;

    void SetOccluders(const Wall* walls, int wallCount, float originX, float originY,
                      float worldWidth, float worldHeight) override;
    void DrawWalls(const Wall* walls, int wallCount) override;
    void DrawLighting(const Light* lights, int lightCount,
                      const TileDrawConstants& camera) override;

    const char* GetBackendName() const override { return "Vulkan"; }

private:
    static constexpr uint32_t kMaxFramesInFlight = 2;

    bool CreateInstance();
    bool CreateSurface(Window& window);
    bool PickPhysicalDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    bool CreateImageViews();
    bool CreateRenderPass();
    bool CreateFramebuffers();
    bool CreateImageSemaphores();
    bool CreatePipeline();
    bool CreateCommandResources();
    bool CreateFrameSyncObjects();

    // Everything that depends on the swapchain's size or image count, so a
    // resize can rebuild exactly that set and leave the rest alone.
    void DestroySwapchainObjects();
    bool RecreateSwapchain();

    // Tile-map objects that survive map rebuilds (sampler, set layout, pool,
    // set, pipeline layout, pipeline). Created once, then cached; resumes from
    // a partial earlier failure. Requires Init() to have completed.
    bool CreateTilePipelineObjects();

    // One sampled image (optimal tiling, device-local) plus its view. Writes
    // whatever it managed to create into the out-params so a failed call can
    // still be cleaned up by the caller's own destroy path. `usage` covers the
    // tile textures (transfer destination) and the occlusion mask (rendered to).
    bool CreateSampledImage(VkFormat format, uint32_t width, uint32_t height,
                            VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory,
                            VkImageView& view);

    // Host-visible, host-coherent TRANSFER_SRC buffer, left persistently mapped.
    bool CreateStagingBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory,
                             void** mapped);

    bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties,
                        uint32_t& typeIndex) const;

    // Per-map objects only (the three images and the staging mirror); the
    // cached pipeline objects stay. Callers wait for the device first.
    void DestroyTileImages();

    // Records the queued tile-id rows into the frame's command buffer. Called
    // by BeginFrame after the fence wait and before the render pass begins.
    void FlushPendingTileUpload(VkCommandBuffer commandBuffer);

    // Lighting objects that survive occluder rebuilds (the mask render pass and
    // occluder pipeline, the sampler, set layout, pool and set, and the light
    // resumes from a partial earlier failure. Requires Init() to have completed.
    bool CreateLightingPipelineObjects();

    // The R8_UNORM occlusion mask plus its view and framebuffer, at the given
    // resolution. Writes whatever it managed to create into the members so a
    // failed call can still be cleaned up with DestroyOcclusionMask().
    bool CreateOcclusionMask(uint32_t width, uint32_t height);

    // Clears the mask and rasterizes every light-blocking wall into it on a
    // one-shot command buffer, then waits for the queue. The render pass leaves
    // the image in SHADER_READ_ONLY_OPTIMAL, so no extra barrier is needed.
    bool RasterizeOccluders(const Wall* walls, int wallCount);

    // Per-mask objects only (image, memory, view, framebuffer); the cached
    // pipeline objects stay. Callers wait for the device first.
    void DestroyOcclusionMask();

    // One pipeline whose vertices come from gl_VertexIndex alone: no vertex
    // input, one color attachment, dynamic viewport and scissor. Shared by the
    // blending and target render pass.
    bool CreateVertexlessPipeline(const char* vertexPath, const char* fragmentPath,
                                  VkPrimitiveTopology topology,
                                  const VkPipelineColorBlendAttachmentState& blend,
                                  VkPipelineLayout layout, VkRenderPass renderPass,
                                  VkPipeline& pipeline);

    VkShaderModule LoadShaderModule(const char* relativePath) const;

    Window* m_window = nullptr;  // borrowed; outlives the renderer

    VkInstance       m_instance            = VK_NULL_HANDLE;
    VkSurfaceKHR     m_surface             = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice      = VK_NULL_HANDLE;
    VkDevice         m_device              = VK_NULL_HANDLE;
    VkQueue          m_graphicsQueue       = VK_NULL_HANDLE;
    uint32_t         m_graphicsQueueFamily = 0;

    VkSwapchainKHR             m_swapchain       = VK_NULL_HANDLE;
    VkFormat                   m_swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D                 m_extent{};
    std::vector<VkImage>       m_swapchainImages;  // owned by the swapchain
    std::vector<VkImageView>   m_swapchainImageViews;
    std::vector<VkFramebuffer> m_framebuffers;

    VkRenderPass     m_renderPass     = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;

    VkCommandPool   m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffers[kMaxFramesInFlight]{};
    VkSemaphore     m_imageAvailableSemaphores[kMaxFramesInFlight]{};
    VkFence         m_inFlightFences[kMaxFramesInFlight]{};

    // One per swapchain image rather than per frame in flight: the semaphore a
    // present waits on stays in use until that image comes back, which is not
    // bounded by the frame slot that signalled it.
    std::vector<VkSemaphore> m_renderFinishedSemaphores;

    // --- Tile map ---------------------------------------------------------
    // Cached across map rebuilds; created lazily by the first
    // CreateTileResources call and destroyed only in Shutdown.
    VkSampler             m_tileSampler        = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_tileSetLayout      = VK_NULL_HANDLE;
    VkDescriptorPool      m_tileDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet       m_tileDescriptorSet  = VK_NULL_HANDLE;  // freed with the pool
    VkPipelineLayout      m_tilePipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_tilePipeline       = VK_NULL_HANDLE;

    // Per-map images, replaced whenever CreateTileResources runs again.
    VkImage        m_tileIdImage   = VK_NULL_HANDLE;  // R16_UINT, one texel per tile
    VkDeviceMemory m_tileIdMemory  = VK_NULL_HANDLE;
    VkImageView    m_tileIdView    = VK_NULL_HANDLE;
    VkImage        m_atlasImage    = VK_NULL_HANDLE;  // RGBA8 tile textures
    VkDeviceMemory m_atlasMemory   = VK_NULL_HANDLE;
    VkImageView    m_atlasView     = VK_NULL_HANDLE;
    VkImage        m_paletteImage  = VK_NULL_HANDLE;  // RGBA32F, 256x2 lookup
    VkDeviceMemory m_paletteMemory = VK_NULL_HANDLE;
    VkImageView    m_paletteView   = VK_NULL_HANDLE;

    // Persistently mapped host-visible mirror of the full tile-id grid.
    // UpdateTileRegion writes into it; BeginFrame copies the dirty rows to the
    // tile-id image before the render pass starts.
    VkBuffer       m_tileStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_tileStagingMemory = VK_NULL_HANDLE;
    void*          m_tileStagingMapped = nullptr;

    int  m_tileMapWidth       = 0;
    int  m_tileMapHeight      = 0;
    bool m_tileResourcesReady = false;  // false makes DrawTileMap a no-op

    // Queued rows awaiting upload, merged conservatively into one row range
    // [begin, end). Whole rows keep the copy's buffer offset trivially aligned.
    bool m_tileUploadPending = false;
    int  m_dirtyRowBegin     = 0;
    int  m_dirtyRowEnd       = 0;

    // --- Volumetric lighting ----------------------------------------------
    // Cached across occluder rebuilds; created lazily by the first
    // pipelines hang off m_renderPass, which survives swapchain recreation, so
    // a resize leaves all of this alone.
    VkRenderPass     m_maskRenderPass         = VK_NULL_HANDLE;  // R8_UNORM, one subpass
    VkPipelineLayout m_occluderPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_occluderPipeline       = VK_NULL_HANDLE;

    VkSampler             m_lightSampler        = VK_NULL_HANDLE;  // linear, clamp to edge
    VkDescriptorSetLayout m_lightSetLayout      = VK_NULL_HANDLE;
    VkDescriptorPool      m_lightDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet       m_lightDescriptorSet  = VK_NULL_HANDLE;  // freed with the pool
    VkPipelineLayout      m_lightPipelineLayout = VK_NULL_HANDLE;  // shared by both pipelines
    VkPipeline            m_lightPipeline       = VK_NULL_HANDLE;  // additive

    // The mask itself, replaced whenever SetOccluders runs.
    VkImage        m_maskImage       = VK_NULL_HANDLE;  // R8_UNORM, 1 = blocks light
    VkDeviceMemory m_maskMemory      = VK_NULL_HANDLE;
    VkImageView    m_maskView        = VK_NULL_HANDLE;
    VkFramebuffer  m_maskFramebuffer = VK_NULL_HANDLE;

    // The world rectangle the mask covers and its resolution. The light shader
    // needs the rectangle to turn a world position into a mask UV.
    float    m_maskOriginX = 0.0f;
    float    m_maskOriginY = 0.0f;
    float    m_maskWorldW  = 1.0f;
    float    m_maskWorldH  = 1.0f;
    uint32_t m_maskWidth   = 0;
    uint32_t m_maskHeight  = 0;
    bool     m_maskReady   = false;  // false makes DrawLighting a no-op

    // The last camera DrawTileMap was given: DrawWalls has no camera of its own
    // and transforms its world-space rectangles with this one.
    TileDrawConstants m_cameraConstants{};

    uint32_t m_frameIndex    = 0;
    uint32_t m_imageIndex    = 0;
    bool     m_frameActive   = false;  // false makes DrawQuad/EndFrame no-ops
    bool     m_needsRecreate = false;
};

} // namespace engine
