#include "renderer/vulkan/VulkanRenderer.h"

#include <cstring>
#include <vector>

// vulkan.h must be included before SDL_vulkan.h so SDL sees the real Vulkan
// types instead of declaring its own opaque stand-ins.
#include <vulkan/vulkan.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_vulkan.h>

#include "core/Log.h"
#include "core/Window.h"

namespace engine {

namespace {

bool HasValidationLayer()
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

VulkanRenderer::~VulkanRenderer()
{
    Shutdown();
}

bool VulkanRenderer::Init(Window& window)
{
    if (!CreateInstance())      return false;
    if (!CreateSurface(window)) return false;
    if (!PickPhysicalDevice())  return false;
    if (!CreateDevice())        return false;
    return true;
}

bool VulkanRenderer::CreateInstance()
{
    // SDL owns the returned array (do not free). Valid to call here because
    // the SDL_WINDOW_VULKAN window already exists when Init() runs.
    Uint32 extensionCount = 0;
    char const* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (!extensions) {
        LOG_ERROR("[Vulkan] SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "Cpp Engine";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName        = "Cpp Engine";
    appInfo.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo        = &appInfo;
    createInfo.enabledExtensionCount   = extensionCount;
    createInfo.ppEnabledExtensionNames = extensions;

#ifndef NDEBUG
    static const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
    if (HasValidationLayer()) {
        createInfo.enabledLayerCount   = 1;
        createInfo.ppEnabledLayerNames = &kValidationLayer;
        LOG_INFO("[Vulkan] Validation layer enabled");
    } else {
        LOG_WARN("[Vulkan] Validation layer not available");
    }
#endif

    const VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateInstance failed (VkResult %d)", static_cast<int>(result));
        return false;
    }
    LOG_INFO("[Vulkan] Instance created (%u instance extension(s))", extensionCount);
    return true;
}

bool VulkanRenderer::CreateSurface(Window& window)
{
    if (!SDL_Vulkan_CreateSurface(window.GetSDLWindow(), m_instance, nullptr, &m_surface)) {
        LOG_ERROR("[Vulkan] SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    LOG_INFO("[Vulkan] Window surface created");
    return true;
}

bool VulkanRenderer::PickPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        LOG_ERROR("[Vulkan] No Vulkan-capable GPU found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    int bestScore = -1;
    for (VkPhysicalDevice device : devices) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

        for (uint32_t family = 0; family < familyCount; ++family) {
            const bool graphics = (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            const bool present  = SDL_Vulkan_GetPresentationSupport(m_instance, device, family);
            if (!graphics || !present) {
                continue;
            }

            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(device, &props);
            const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > bestScore) {
                bestScore             = score;
                m_physicalDevice      = device;
                m_graphicsQueueFamily = family;
            }
            break;  // first suitable family of this device is enough
        }
    }

    if (m_physicalDevice == VK_NULL_HANDLE) {
        LOG_ERROR("[Vulkan] No GPU with a graphics+present queue family found");
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    LOG_INFO("[Vulkan] Physical device: %s (queue family %u)",
             props.deviceName, m_graphicsQueueFamily);
    return true;
}

bool VulkanRenderer::CreateDevice()
{
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = m_graphicsQueueFamily;
    queueInfo.queueCount       = 1;
    queueInfo.pQueuePriorities = &priority;

    // Enabled now because device extensions cannot be added after creation;
    // the swapchain itself is future work.
    const char* kDeviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount    = 1;
    createInfo.pQueueCreateInfos       = &queueInfo;
    createInfo.enabledExtensionCount   = 1;
    createInfo.ppEnabledExtensionNames = kDeviceExtensions;

    const VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateDevice failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
    LOG_INFO("[Vulkan] Logical device and graphics queue created");
    return true;
}

void VulkanRenderer::Shutdown()
{
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device        = VK_NULL_HANDLE;
        m_graphicsQueue = VK_NULL_HANDLE;
    }
    if (m_surface != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
    m_physicalDevice = VK_NULL_HANDLE;
}

} // namespace engine
