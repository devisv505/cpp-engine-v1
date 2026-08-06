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

    uint32_t m_frameIndex    = 0;
    uint32_t m_imageIndex    = 0;
    bool     m_frameActive   = false;  // false makes DrawQuad/EndFrame no-ops
    bool     m_needsRecreate = false;
};

} // namespace engine
