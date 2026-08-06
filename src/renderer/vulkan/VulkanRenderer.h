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

    bool InitImGui(Window& window) override;
    void BeginImGuiFrame() override;
    void RenderImGui() override;
    void ShutdownImGui() override;

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
    // still be cleaned up with DestroyTileImages().
    bool CreateTileImage(VkFormat format, uint32_t width, uint32_t height,
                         VkImage& image, VkDeviceMemory& memory, VkImageView& view);

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

    // --- Dear ImGui -------------------------------------------------------
    VkDescriptorPool m_imguiDescriptorPool = VK_NULL_HANDLE;
    bool             m_imguiInitialized    = false;

    uint32_t m_frameIndex    = 0;
    uint32_t m_imageIndex    = 0;
    bool     m_frameActive   = false;  // false makes DrawQuad/EndFrame no-ops
    bool     m_needsRecreate = false;
};

} // namespace engine
