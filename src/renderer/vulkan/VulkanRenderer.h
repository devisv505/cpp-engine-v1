#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "renderer/IRenderer.h"

namespace engine {

class VulkanRenderer final : public IRenderer {
public:
    ~VulkanRenderer() override;

    bool Init(Window& window) override;
    void Shutdown() override;
    const char* GetBackendName() const override { return "Vulkan"; }

private:
    bool CreateInstance();
    bool CreateSurface(Window& window);
    bool PickPhysicalDevice();
    bool CreateDevice();

    VkInstance       m_instance            = VK_NULL_HANDLE;
    VkSurfaceKHR     m_surface             = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice      = VK_NULL_HANDLE;
    VkDevice         m_device              = VK_NULL_HANDLE;
    VkQueue          m_graphicsQueue       = VK_NULL_HANDLE;
    uint32_t         m_graphicsQueueFamily = 0;
};

} // namespace engine
