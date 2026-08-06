#include "renderer/vulkan/VulkanRenderer.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// vulkan.h must be included before SDL_vulkan.h so SDL sees the real Vulkan
// types instead of declaring its own opaque stand-ins.
#include <vulkan/vulkan.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_vulkan.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include "core/Log.h"
#include "core/Window.h"
#include "world/TileAtlas.h"

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

bool HasPresentMode(const std::vector<VkPresentModeKHR>& modes, VkPresentModeKHR mode)
{
    return std::find(modes.begin(), modes.end(), mode) != modes.end();
}

// FIFO is the only mode every implementation must support, and it is also the
// vsync-on mode, so it doubles as the fallback.
VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes, bool vsync)
{
    if (vsync) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    if (HasPresentMode(modes, VK_PRESENT_MODE_MAILBOX_KHR)) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (HasPresentMode(modes, VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

const char* PresentModeName(VkPresentModeKHR mode)
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR:      return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR:         return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    default:                               return "unknown";
    }
}

// The tile shaders and every backend agree on this exact push-constant size.
static_assert(sizeof(TileDrawConstants) == 64, "TileDrawConstants must stay 64 bytes");

// Single-mip, single-layer color barrier; the caller picks the pipeline stages.
VkImageMemoryBarrier MakeImageBarrier(VkImage image, VkImageLayout oldLayout,
                                      VkImageLayout newLayout, VkAccessFlags srcAccess,
                                      VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask                   = srcAccess;
    barrier.dstAccessMask                   = dstAccess;
    barrier.oldLayout                       = oldLayout;
    barrier.newLayout                       = newLayout;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = image;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;
    return barrier;
}

} // namespace

VulkanRenderer::~VulkanRenderer()
{
    Shutdown();
}

bool VulkanRenderer::Init(Window& window)
{
    m_window = &window;

    if (!CreateInstance())         return false;
    if (!CreateSurface(window))    return false;
    if (!PickPhysicalDevice())     return false;
    if (!CreateDevice())           return false;
    if (!CreateSwapchain())        return false;
    if (!CreateImageViews())       return false;
    if (!CreateRenderPass())       return false;
    if (!CreateFramebuffers())     return false;
    if (!CreateImageSemaphores())  return false;
    if (!CreatePipeline())         return false;
    if (!CreateCommandResources()) return false;
    if (!CreateFrameSyncObjects()) return false;
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

bool VulkanRenderer::CreateSwapchain()
{
    VkSurfaceCapabilitiesKHR caps{};
    const VkResult capsResult =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &caps);
    if (capsResult != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (VkResult %d)",
                  static_cast<int>(capsResult));
        return false;
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
    if (formatCount == 0) {
        LOG_ERROR("[Vulkan] Surface reports no supported formats");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());

    // Shader colours are written as-is, so an UNORM target keeps them linear;
    // any format the surface offers still beats failing to create a swapchain.
    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = format;
            break;
        }
    }

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(modeCount);
    if (modeCount > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount,
                                                  presentModes.data());
    }
    const VkPresentModeKHR presentMode =
        ChoosePresentMode(presentModes, m_window->GetConfig().vsync);

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        // The surface leaves the size to us; use the real framebuffer size.
        int width  = 0;
        int height = 0;
        m_window->GetPixelSize(width, height);
        extent.width  = std::clamp(static_cast<uint32_t>(std::max(width, 0)),
                                   caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(std::max(height, 0)),
                                   caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        LOG_WARN("[Vulkan] Surface extent is zero (minimized); skipping swapchain creation");
        return false;
    }

    // One image beyond the minimum so the CPU is not stuck waiting on the
    // presentation engine to hand an image back.
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface          = m_surface;
    createInfo.minImageCount    = imageCount;
    createInfo.imageFormat      = surfaceFormat.format;
    createInfo.imageColorSpace  = surfaceFormat.colorSpace;
    createInfo.imageExtent      = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;  // one family draws and presents
    createInfo.preTransform     = caps.currentTransform;
    createInfo.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode      = presentMode;
    createInfo.clipped          = VK_TRUE;
    createInfo.oldSwapchain     = VK_NULL_HANDLE;  // the old one is destroyed before we get here

    const VkResult result = vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateSwapchainKHR failed (VkResult %d)", static_cast<int>(result));
        m_swapchain = VK_NULL_HANDLE;
        return false;
    }

    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &actualCount, nullptr);
    m_swapchainImages.resize(actualCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &actualCount, m_swapchainImages.data());

    m_swapchainFormat = surfaceFormat.format;
    m_extent          = extent;

    LOG_INFO("[Vulkan] Swapchain created (%ux%u, %u images, %s)",
             extent.width, extent.height, actualCount, PresentModeName(presentMode));
    return true;
}

bool VulkanRenderer::CreateImageViews()
{
    m_swapchainImageViews.assign(m_swapchainImages.size(), VK_NULL_HANDLE);

    for (size_t i = 0; i < m_swapchainImages.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image                           = m_swapchainImages[i];
        createInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format                          = m_swapchainFormat;
        createInfo.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel   = 0;
        createInfo.subresourceRange.levelCount     = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount     = 1;

        const VkResult result =
            vkCreateImageView(m_device, &createInfo, nullptr, &m_swapchainImageViews[i]);
        if (result != VK_SUCCESS) {
            LOG_ERROR("[Vulkan] vkCreateImageView failed (VkResult %d)", static_cast<int>(result));
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::CreateRenderPass()
{
    VkAttachmentDescription color{};
    color.format         = m_swapchainFormat;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;  // cleared every frame anyway
    color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    // Holds the implicit UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL transition back
    // until the acquire semaphore has been waited on at the same stage.
    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo createInfo{};
    createInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = 1;
    createInfo.pAttachments    = &color;
    createInfo.subpassCount    = 1;
    createInfo.pSubpasses      = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies   = &dependency;

    const VkResult result = vkCreateRenderPass(m_device, &createInfo, nullptr, &m_renderPass);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateRenderPass failed (VkResult %d)", static_cast<int>(result));
        m_renderPass = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool VulkanRenderer::CreateFramebuffers()
{
    m_framebuffers.assign(m_swapchainImageViews.size(), VK_NULL_HANDLE);

    for (size_t i = 0; i < m_swapchainImageViews.size(); ++i) {
        VkFramebufferCreateInfo createInfo{};
        createInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass      = m_renderPass;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments    = &m_swapchainImageViews[i];
        createInfo.width           = m_extent.width;
        createInfo.height          = m_extent.height;
        createInfo.layers          = 1;

        const VkResult result =
            vkCreateFramebuffer(m_device, &createInfo, nullptr, &m_framebuffers[i]);
        if (result != VK_SUCCESS) {
            LOG_ERROR("[Vulkan] vkCreateFramebuffer failed (VkResult %d)",
                      static_cast<int>(result));
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::CreateImageSemaphores()
{
    VkSemaphoreCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    m_renderFinishedSemaphores.assign(m_swapchainImages.size(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : m_renderFinishedSemaphores) {
        if (vkCreateSemaphore(m_device, &createInfo, nullptr, &semaphore) != VK_SUCCESS) {
            LOG_ERROR("[Vulkan] vkCreateSemaphore failed for a render-finished semaphore");
            return false;
        }
    }
    return true;
}

VkShaderModule VulkanRenderer::LoadShaderModule(const char* relativePath) const
{
    // Compiled SPIR-V is copied next to the executable by the build, so it is
    // found no matter which directory the app is launched from.
    std::string path = relativePath;
    if (const char* basePath = SDL_GetBasePath()) {
        path = std::string(basePath) + relativePath;
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        LOG_ERROR("[Vulkan] Shader not found: %s", path.c_str());
        return VK_NULL_HANDLE;
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size <= 0 || size % 4 != 0) {
        LOG_ERROR("[Vulkan] %s is not valid SPIR-V (%d bytes)", path.c_str(),
                  static_cast<int>(size));
        return VK_NULL_HANDLE;
    }

    // uint32_t storage also gives pCode the 4-byte alignment Vulkan requires.
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    if (!file.read(reinterpret_cast<char*>(code.data()), size)) {
        LOG_ERROR("[Vulkan] Failed to read %s", path.c_str());
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = static_cast<size_t>(size);  // bytes, not words
    createInfo.pCode    = code.data();

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateShaderModule failed for %s (VkResult %d)", path.c_str(),
                  static_cast<int>(result));
        return VK_NULL_HANDLE;
    }
    return shaderModule;
}

bool VulkanRenderer::CreatePipeline()
{
    VkShaderModule vertexModule   = LoadShaderModule("shaders/quad.vert.spv");
    VkShaderModule fragmentModule = LoadShaderModule("shaders/quad.frag.spv");
    if (vertexModule == VK_NULL_HANDLE || fragmentModule == VK_NULL_HANDLE) {
        if (vertexModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, vertexModule, nullptr);
        }
        if (fragmentModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, fragmentModule, nullptr);
        }
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(QuadConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    VkResult result = vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreatePipelineLayout failed (VkResult %d)",
                  static_cast<int>(result));
        m_pipelineLayout = VK_NULL_HANDLE;
        vkDestroyShaderModule(m_device, vertexModule, nullptr);
        vkDestroyShaderModule(m_device, fragmentModule, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentModule;
    stages[1].pName  = "main";

    // No bindings and no attributes: the vertex shader builds the four corners
    // from gl_VertexIndex and the push constants.
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Counts must still be 1 even though both are dynamic.
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable        = VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;
    raster.polygonMode             = VK_POLYGON_MODE_FILL;
    raster.cullMode                = VK_CULL_MODE_NONE;
    raster.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable         = VK_FALSE;
    raster.lineWidth               = 1.0f;  // required, and 1.0 without wideLines

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisample.sampleShadingEnable  = VK_FALSE;
    multisample.minSampleShading     = 1.0f;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable         = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.logicOpEnable   = VK_FALSE;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttachment;

    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState   = &multisample;
    pipelineInfo.pDepthStencilState  = nullptr;  // the subpass has no depth attachment
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_pipelineLayout;
    pipelineInfo.renderPass          = m_renderPass;
    pipelineInfo.subpass             = 0;
    pipelineInfo.basePipelineHandle  = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex   = -1;

    result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                       &m_pipeline);

    // The modules are only needed while the pipeline is being compiled.
    vkDestroyShaderModule(m_device, vertexModule, nullptr);
    vkDestroyShaderModule(m_device, fragmentModule, nullptr);

    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateGraphicsPipelines failed (VkResult %d)",
                  static_cast<int>(result));
        m_pipeline = VK_NULL_HANDLE;
        return false;
    }
    LOG_INFO("[Vulkan] Quad pipeline created");
    return true;
}

bool VulkanRenderer::CreateCommandResources()
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

    VkResult result = vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateCommandPool failed (VkResult %d)", static_cast<int>(result));
        m_commandPool = VK_NULL_HANDLE;
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = m_commandPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kMaxFramesInFlight;

    result = vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkAllocateCommandBuffers failed (VkResult %d)",
                  static_cast<int>(result));
        return false;
    }
    return true;
}

bool VulkanRenderer::CreateFrameSyncObjects()
{
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // so the first frame does not block

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr,
                              &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS) {
            LOG_ERROR("[Vulkan] Failed to create per-frame synchronization objects");
            return false;
        }
    }
    return true;
}

void VulkanRenderer::BeginFrame(const Color& clearColor)
{
    m_frameActive = false;
    if (m_device == VK_NULL_HANDLE) {
        return;
    }

    if (m_needsRecreate && !RecreateSwapchain()) {
        return;  // minimized or failed; try again next frame
    }
    if (m_swapchain == VK_NULL_HANDLE) {
        return;
    }

    vkWaitForFences(m_device, 1, &m_inFlightFences[m_frameIndex], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    const VkResult acquired =
        vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                              m_imageAvailableSemaphores[m_frameIndex], VK_NULL_HANDLE,
                              &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        m_needsRecreate = true;
        return;  // nothing was acquired and no semaphore was signalled
    }
    if (acquired == VK_SUBOPTIMAL_KHR) {
        // An image *was* acquired and the semaphore *was* signalled, so this
        // frame has to be rendered: skipping it would leave the semaphore
        // signalled with nothing waiting on it. Rebuild at the next frame.
        m_needsRecreate = true;
    } else if (acquired != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkAcquireNextImageKHR failed (VkResult %d)",
                  static_cast<int>(acquired));
        return;
    }

    m_imageIndex = imageIndex;
    VkCommandBuffer commandBuffer = m_commandBuffers[m_frameIndex];
    vkResetCommandBuffer(commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkBeginCommandBuffer failed");
        return;
    }

    // Tile edits queued since the last frame go out first, while no render
    // pass is active: transfers are not allowed inside one.
    FlushPendingTileUpload(commandBuffer);

    VkClearValue clear{};
    clear.color.float32[0] = clearColor.r;
    clear.color.float32[1] = clearColor.g;
    clear.color.float32[2] = clearColor.b;
    clear.color.float32[3] = clearColor.a;

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass        = m_renderPass;
    passInfo.framebuffer       = m_framebuffers[m_imageIndex];
    passInfo.renderArea.offset = {0, 0};
    passInfo.renderArea.extent = m_extent;
    passInfo.clearValueCount   = 1;
    passInfo.pClearValues      = &clear;
    vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(m_extent.width);
    viewport.height   = static_cast<float>(m_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_extent;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    m_frameActive = true;
}

void VulkanRenderer::DrawQuad(const Quad& quad)
{
    if (!m_frameActive) {
        return;
    }

    QuadConstants constants{};
    constants.rect[0]     = quad.x;
    constants.rect[1]     = quad.y;
    constants.rect[2]     = quad.w;
    constants.rect[3]     = quad.h;
    constants.color[0]    = quad.color.r;
    constants.color[1]    = quad.color.g;
    constants.color[2]    = quad.color.b;
    constants.color[3]    = quad.color.a;
    constants.viewport[0] = static_cast<float>(m_extent.width);
    constants.viewport[1] = static_cast<float>(m_extent.height);

    VkCommandBuffer commandBuffer = m_commandBuffers[m_frameIndex];
    vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(QuadConstants), &constants);
    vkCmdDraw(commandBuffer, 4, 1, 0, 0);
}

void VulkanRenderer::EndFrame()
{
    if (!m_frameActive) {
        return;
    }
    m_frameActive = false;

    VkCommandBuffer commandBuffer = m_commandBuffers[m_frameIndex];
    vkCmdEndRenderPass(commandBuffer);
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkEndCommandBuffer failed");
        return;
    }

    VkSemaphore waitSemaphore   = m_imageAvailableSemaphores[m_frameIndex];
    VkSemaphore signalSemaphore = m_renderFinishedSemaphores[m_imageIndex];
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount   = 1;
    submitInfo.pWaitSemaphores      = &waitSemaphore;
    submitInfo.pWaitDstStageMask    = &waitStage;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &signalSemaphore;

    // Reset as late as possible: every path that bails out earlier leaves the
    // fence signalled, so the next frame's wait cannot deadlock.
    vkResetFences(m_device, 1, &m_inFlightFences[m_frameIndex]);
    const VkResult submitted =
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_frameIndex]);
    if (submitted != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkQueueSubmit failed (VkResult %d)", static_cast<int>(submitted));
        return;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = &signalSemaphore;
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = &m_swapchain;
    presentInfo.pImageIndices      = &m_imageIndex;

    const VkResult presented = vkQueuePresentKHR(m_graphicsQueue, &presentInfo);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        m_needsRecreate = true;
    } else if (presented != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkQueuePresentKHR failed (VkResult %d)", static_cast<int>(presented));
    }

    m_frameIndex = (m_frameIndex + 1) % kMaxFramesInFlight;
}

// --- Tile map ---------------------------------------------------------------

bool VulkanRenderer::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties,
                                    uint32_t& typeIndex) const
{
    VkPhysicalDeviceMemoryProperties memoryProps{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProps);
    for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 &&
            (memoryProps.memoryTypes[i].propertyFlags & properties) == properties) {
            typeIndex = i;
            return true;
        }
    }
    LOG_ERROR("[Vulkan] No memory type with properties 0x%x", properties);
    return false;
}

bool VulkanRenderer::CreateTileImage(VkFormat format, uint32_t width, uint32_t height,
                                     VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = format;
    imageInfo.extent        = {width, height, 1};
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = vkCreateImage(m_device, &imageInfo, nullptr, &image);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateImage failed for a %ux%u tile image (VkResult %d)",
                  width, height, static_cast<int>(result));
        image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_device, image, &requirements);

    uint32_t typeIndex = 0;
    if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        typeIndex)) {
        return false;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = requirements.size;
    allocInfo.memoryTypeIndex = typeIndex;

    result = vkAllocateMemory(m_device, &allocInfo, nullptr, &memory);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkAllocateMemory failed for a tile image (VkResult %d)",
                  static_cast<int>(result));
        memory = VK_NULL_HANDLE;
        return false;
    }
    result = vkBindImageMemory(m_device, image, memory, 0);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkBindImageMemory failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = image;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = format;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    result = vkCreateImageView(m_device, &viewInfo, nullptr, &view);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateImageView failed for a tile image (VkResult %d)",
                  static_cast<int>(result));
        view = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool VulkanRenderer::CreateStagingBuffer(VkDeviceSize size, VkBuffer& buffer,
                                         VkDeviceMemory& memory, void** mapped)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size        = size;
    bufferInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateBuffer failed for a %u-byte staging buffer (VkResult %d)",
                  static_cast<uint32_t>(size), static_cast<int>(result));
        buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, buffer, &requirements);

    uint32_t typeIndex = 0;
    if (!FindMemoryType(requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        typeIndex)) {
        return false;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = requirements.size;
    allocInfo.memoryTypeIndex = typeIndex;

    result = vkAllocateMemory(m_device, &allocInfo, nullptr, &memory);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkAllocateMemory failed for a staging buffer (VkResult %d)",
                  static_cast<int>(result));
        memory = VK_NULL_HANDLE;
        return false;
    }
    result = vkBindBufferMemory(m_device, buffer, memory, 0);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkBindBufferMemory failed (VkResult %d)", static_cast<int>(result));
        return false;
    }
    result = vkMapMemory(m_device, memory, 0, VK_WHOLE_SIZE, 0, mapped);
    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkMapMemory failed (VkResult %d)", static_cast<int>(result));
        *mapped = nullptr;
        return false;
    }
    return true;
}

bool VulkanRenderer::CreateTilePipelineObjects()
{
    // Cached across map rebuilds: only the per-map images change. Each step
    // checks its own handle so a partial earlier failure resumes cleanly.
    if (m_tilePipeline != VK_NULL_HANDLE) {
        return true;
    }

    if (m_tileSampler == VK_NULL_HANDLE) {
        // Nearest + clamp for all three textures. Tile ids and the palette are
        // read with texelFetch (the sampler never runs); the atlas wants hard
        // texel edges anyway.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter     = VK_FILTER_NEAREST;
        samplerInfo.minFilter     = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxAnisotropy = 1.0f;

        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_tileSampler) != VK_SUCCESS) {
            LOG_ERROR("[Vulkan] vkCreateSampler failed for the tile sampler");
            m_tileSampler = VK_NULL_HANDLE;
            return false;
        }
    }

    if (m_tileSetLayout == VK_NULL_HANDLE) {
        // 0 = tile ids (R16_UINT), 1 = atlas (RGBA8), 2 = palette (RGBA32F).
        VkDescriptorSetLayoutBinding bindings[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings    = bindings;

        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_tileSetLayout) !=
            VK_SUCCESS) {
            LOG_ERROR("[Vulkan] vkCreateDescriptorSetLayout failed for the tile set layout");
            m_tileSetLayout = VK_NULL_HANDLE;
            return false;
        }
    }

    if (m_tileDescriptorPool == VK_NULL_HANDLE) {
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 3;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets       = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;

        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_tileDescriptorPool) !=
            VK_SUCCESS) {
            LOG_ERROR("[Vulkan] vkCreateDescriptorPool failed for the tile pool");
            m_tileDescriptorPool = VK_NULL_HANDLE;
            return false;
        }
    }

    if (m_tileDescriptorSet == VK_NULL_HANDLE) {
        // Allocated once and re-pointed at new images with vkUpdateDescriptorSets
        // on every map rebuild (the device is idle at that point).
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_tileDescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_tileSetLayout;

        if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_tileDescriptorSet) != VK_SUCCESS) {
            LOG_ERROR("[Vulkan] vkAllocateDescriptorSets failed for the tile set");
            m_tileDescriptorSet = VK_NULL_HANDLE;
            return false;
        }
    }

    if (m_tilePipelineLayout == VK_NULL_HANDLE) {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset     = 0;
        pushRange.size       = sizeof(TileDrawConstants);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount         = 1;
        layoutInfo.pSetLayouts            = &m_tileSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &pushRange;

        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_tilePipelineLayout) !=
            VK_SUCCESS) {
            LOG_ERROR("[Vulkan] vkCreatePipelineLayout failed for the tile pipeline");
            m_tilePipelineLayout = VK_NULL_HANDLE;
            return false;
        }
    }

    VkShaderModule vertexModule   = LoadShaderModule("shaders/tile.vert.spv");
    VkShaderModule fragmentModule = LoadShaderModule("shaders/tile.frag.spv");
    if (vertexModule == VK_NULL_HANDLE || fragmentModule == VK_NULL_HANDLE) {
        if (vertexModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, vertexModule, nullptr);
        }
        if (fragmentModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, fragmentModule, nullptr);
        }
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentModule;
    stages[1].pName  = "main";

    // No inputs: the vertex shader emits one fullscreen triangle from
    // gl_VertexIndex alone.
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable        = VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;
    raster.polygonMode             = VK_POLYGON_MODE_FILL;
    raster.cullMode                = VK_CULL_MODE_NONE;
    raster.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable         = VK_FALSE;
    raster.lineWidth               = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisample.sampleShadingEnable  = VK_FALSE;
    multisample.minSampleShading     = 1.0f;

    // The tile pass is opaque (it fills every covered pixel with alpha 1), so
    // blending stays off, unlike the quad pipeline.
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable    = VK_FALSE;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.logicOpEnable   = VK_FALSE;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttachment;

    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState   = &multisample;
    pipelineInfo.pDepthStencilState  = nullptr;
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_tilePipelineLayout;
    pipelineInfo.renderPass          = m_renderPass;
    pipelineInfo.subpass             = 0;
    pipelineInfo.basePipelineHandle  = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex   = -1;

    const VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                      nullptr, &m_tilePipeline);

    vkDestroyShaderModule(m_device, vertexModule, nullptr);
    vkDestroyShaderModule(m_device, fragmentModule, nullptr);

    if (result != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateGraphicsPipelines failed for the tile pipeline (VkResult %d)",
                  static_cast<int>(result));
        m_tilePipeline = VK_NULL_HANDLE;
        return false;
    }
    LOG_INFO("[Vulkan] Tile pipeline created");
    return true;
}

bool VulkanRenderer::CreateTileResources(const TileRenderData& data, int mapWidth, int mapHeight,
                                         const uint16_t* tiles)
{
    if (m_device == VK_NULL_HANDLE) {
        return false;
    }
    if (mapWidth <= 0 || mapHeight <= 0 || tiles == nullptr) {
        LOG_ERROR("[Vulkan] CreateTileResources: invalid map (%dx%d)", mapWidth, mapHeight);
        return false;
    }

    const size_t atlasBytes   = static_cast<size_t>(data.atlasWidth) *
                                static_cast<size_t>(data.atlasHeight) * 4;
    const size_t paletteBytes = static_cast<size_t>(TileRegistry::kMaxTileTypes) * 2 * 4 *
                                sizeof(float);
    if (data.atlasWidth <= 0 || data.atlasHeight <= 0 ||
        data.atlasPixels.size() < atlasBytes ||
        data.palettePixels.size() * sizeof(float) < paletteBytes) {
        LOG_ERROR("[Vulkan] CreateTileResources: render data does not match its dimensions");
        return false;
    }

    // In-flight frames may still be sampling the previous map's images.
    vkDeviceWaitIdle(m_device);
    DestroyTileImages();

    if (!CreateTilePipelineObjects()) {
        return false;
    }

    const uint32_t mapW = static_cast<uint32_t>(mapWidth);
    const uint32_t mapH = static_cast<uint32_t>(mapHeight);
    if (!CreateTileImage(VK_FORMAT_R16_UINT, mapW, mapH,
                         m_tileIdImage, m_tileIdMemory, m_tileIdView) ||
        !CreateTileImage(VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(data.atlasWidth),
                         static_cast<uint32_t>(data.atlasHeight),
                         m_atlasImage, m_atlasMemory, m_atlasView) ||
        !CreateTileImage(VK_FORMAT_R32G32B32A32_SFLOAT, TileRegistry::kMaxTileTypes, 2,
                         m_paletteImage, m_paletteMemory, m_paletteView)) {
        DestroyTileImages();
        return false;
    }

    // The persistent staging buffer mirrors the whole tile-id grid and also
    // serves as the source for the initial id upload.
    const VkDeviceSize tileBytes = static_cast<VkDeviceSize>(mapW) * mapH * sizeof(uint16_t);
    if (!CreateStagingBuffer(tileBytes, m_tileStagingBuffer, m_tileStagingMemory,
                             &m_tileStagingMapped)) {
        DestroyTileImages();
        return false;
    }
    std::memcpy(m_tileStagingMapped, tiles, static_cast<size_t>(tileBytes));

    // Atlas and palette upload through a throwaway staging buffer; 16-byte
    // alignment satisfies the RGBA32F texel-size requirement on bufferOffset.
    const VkDeviceSize paletteOffset = (static_cast<VkDeviceSize>(atlasBytes) + 15) & ~VkDeviceSize{15};
    VkBuffer       uploadBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
    void*          uploadMapped = nullptr;

    const auto failUpload = [&]() -> bool {
        if (uploadBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, uploadBuffer, nullptr);
        }
        if (uploadMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, uploadMemory, nullptr);  // implicitly unmaps
        }
        DestroyTileImages();
        return false;
    };

    if (!CreateStagingBuffer(paletteOffset + paletteBytes, uploadBuffer, uploadMemory,
                             &uploadMapped)) {
        return failUpload();
    }
    std::memcpy(static_cast<uint8_t*>(uploadMapped), data.atlasPixels.data(), atlasBytes);
    std::memcpy(static_cast<uint8_t*>(uploadMapped) + paletteOffset, data.palettePixels.data(),
                paletteBytes);
    vkUnmapMemory(m_device, uploadMemory);
    uploadMapped = nullptr;

    // One-shot command buffer: transition all three images to TRANSFER_DST,
    // copy, transition to SHADER_READ_ONLY, and wait for the queue.
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool        = m_commandPool;
    cmdAllocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer uploadCmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &uploadCmd) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkAllocateCommandBuffers failed for the tile upload");
        return failUpload();
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(uploadCmd, &beginInfo) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkBeginCommandBuffer failed for the tile upload");
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &uploadCmd);
        return failUpload();
    }

    const VkImageMemoryBarrier toTransfer[3] = {
        MakeImageBarrier(m_tileIdImage, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
        MakeImageBarrier(m_atlasImage, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
        MakeImageBarrier(m_paletteImage, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    vkCmdPipelineBarrier(uploadCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 3, toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset      = 0;
    region.bufferRowLength   = 0;  // tightly packed
    region.bufferImageHeight = 0;
    region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset       = {0, 0, 0};

    region.imageExtent = {mapW, mapH, 1};
    vkCmdCopyBufferToImage(uploadCmd, m_tileStagingBuffer, m_tileIdImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    region.imageExtent = {static_cast<uint32_t>(data.atlasWidth),
                          static_cast<uint32_t>(data.atlasHeight), 1};
    vkCmdCopyBufferToImage(uploadCmd, uploadBuffer, m_atlasImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    region.bufferOffset = paletteOffset;
    region.imageExtent  = {TileRegistry::kMaxTileTypes, 2, 1};
    vkCmdCopyBufferToImage(uploadCmd, uploadBuffer, m_paletteImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    const VkImageMemoryBarrier toSampled[3] = {
        MakeImageBarrier(m_tileIdImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        MakeImageBarrier(m_atlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        MakeImageBarrier(m_paletteImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
    };
    vkCmdPipelineBarrier(uploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3,
                         toSampled);

    bool submitted = vkEndCommandBuffer(uploadCmd) == VK_SUCCESS;
    if (submitted) {
        VkSubmitInfo submitInfo{};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &uploadCmd;
        submitted = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS;
        if (submitted) {
            vkQueueWaitIdle(m_graphicsQueue);
        }
    }
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &uploadCmd);
    vkDestroyBuffer(m_device, uploadBuffer, nullptr);
    vkFreeMemory(m_device, uploadMemory, nullptr);
    uploadBuffer = VK_NULL_HANDLE;
    uploadMemory = VK_NULL_HANDLE;
    if (!submitted) {
        LOG_ERROR("[Vulkan] Tile upload submission failed");
        return failUpload();
    }

    // Point the cached descriptor set at the new images. Safe: the device is
    // idle, so no command buffer still reads the old bindings.
    VkDescriptorImageInfo imageInfos[3]{};
    imageInfos[0].sampler     = m_tileSampler;
    imageInfos[0].imageView   = m_tileIdView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].sampler     = m_tileSampler;
    imageInfos[1].imageView   = m_atlasView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[2].sampler     = m_tileSampler;
    imageInfos[2].imageView   = m_paletteView;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_tileDescriptorSet;
        writes[i].dstBinding      = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo      = &imageInfos[i];
    }
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);

    m_tileMapWidth       = mapWidth;
    m_tileMapHeight      = mapHeight;
    m_tileResourcesReady = true;

    LOG_INFO("[Vulkan] Tile resources created (map %dx%d, atlas %dx%d)",
             mapWidth, mapHeight, data.atlasWidth, data.atlasHeight);
    return true;
}

void VulkanRenderer::UpdateTileRegion(int x, int y, int w, int h, const uint16_t* tiles,
                                      int mapWidth)
{
    if (!m_tileResourcesReady || m_tileStagingMapped == nullptr || tiles == nullptr) {
        return;
    }
    if (mapWidth != m_tileMapWidth) {
        LOG_WARN("[Vulkan] UpdateTileRegion: map width %d does not match resources (%d)",
                 mapWidth, m_tileMapWidth);
        return;
    }

    const int x0 = std::max(x, 0);
    const int y0 = std::max(y, 0);
    const int x1 = std::min(x + w, m_tileMapWidth);
    const int y1 = std::min(y + h, m_tileMapHeight);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    // Only the CPU mirror changes here; the GPU copy is recorded by the next
    // BeginFrame, after that frame's fence wait.
    uint16_t* mirror = static_cast<uint16_t*>(m_tileStagingMapped);
    for (int row = y0; row < y1; ++row) {
        std::memcpy(mirror + static_cast<size_t>(row) * m_tileMapWidth + x0,
                    tiles + static_cast<size_t>(row) * mapWidth + x0,
                    static_cast<size_t>(x1 - x0) * sizeof(uint16_t));
    }

    if (m_tileUploadPending) {
        m_dirtyRowBegin = std::min(m_dirtyRowBegin, y0);
        m_dirtyRowEnd   = std::max(m_dirtyRowEnd, y1);
    } else {
        m_dirtyRowBegin     = y0;
        m_dirtyRowEnd       = y1;
        m_tileUploadPending = true;
    }
}

void VulkanRenderer::FlushPendingTileUpload(VkCommandBuffer commandBuffer)
{
    if (!m_tileUploadPending || !m_tileResourcesReady) {
        return;
    }
    m_tileUploadPending = false;

    // Whole rows only: the mirror holds the full map, so the extra columns are
    // identical bytes, and the copy's bufferOffset stays trivially valid.
    // Starting on an even texel additionally keeps the offset 4-byte aligned.
    int rowBegin = m_dirtyRowBegin;
    if (((rowBegin * m_tileMapWidth) & 1) != 0) {
        --rowBegin;  // an odd product implies rowBegin >= 1
    }
    const int rowCount = m_dirtyRowEnd - rowBegin;

    const VkImageMemoryBarrier toTransfer =
        MakeImageBarrier(m_tileIdImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset      = static_cast<VkDeviceSize>(rowBegin) * m_tileMapWidth *
                               sizeof(uint16_t);
    region.bufferRowLength   = static_cast<uint32_t>(m_tileMapWidth);
    region.bufferImageHeight = 0;
    region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset       = {0, rowBegin, 0};
    region.imageExtent       = {static_cast<uint32_t>(m_tileMapWidth),
                                static_cast<uint32_t>(rowCount), 1};
    vkCmdCopyBufferToImage(commandBuffer, m_tileStagingBuffer, m_tileIdImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    const VkImageMemoryBarrier toSampled =
        MakeImageBarrier(m_tileIdImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toSampled);
}

void VulkanRenderer::DrawTileMap(const TileDrawConstants& constants)
{
    if (!m_frameActive || !m_tileResourcesReady || m_tilePipeline == VK_NULL_HANDLE) {
        return;
    }

    VkCommandBuffer commandBuffer = m_commandBuffers[m_frameIndex];
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilePipelineLayout,
                            0, 1, &m_tileDescriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_tilePipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(TileDrawConstants), &constants);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    // BeginFrame bound the quad pipeline; restore it so DrawQuad keeps working
    // after the tile pass without knowing it ran.
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
}

void VulkanRenderer::DestroyTileImages()
{
    if (m_device == VK_NULL_HANDLE) {
        return;
    }

    if (m_tileStagingMapped != nullptr) {
        vkUnmapMemory(m_device, m_tileStagingMemory);
        m_tileStagingMapped = nullptr;
    }
    if (m_tileStagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_tileStagingBuffer, nullptr);
        m_tileStagingBuffer = VK_NULL_HANDLE;
    }
    if (m_tileStagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_tileStagingMemory, nullptr);
        m_tileStagingMemory = VK_NULL_HANDLE;
    }

    VkImageView* views[]   = { &m_tileIdView, &m_atlasView, &m_paletteView };
    VkImage* images[]      = { &m_tileIdImage, &m_atlasImage, &m_paletteImage };
    VkDeviceMemory* mems[] = { &m_tileIdMemory, &m_atlasMemory, &m_paletteMemory };
    for (int i = 0; i < 3; ++i) {
        if (*views[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, *views[i], nullptr);
            *views[i] = VK_NULL_HANDLE;
        }
        if (*images[i] != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, *images[i], nullptr);
            *images[i] = VK_NULL_HANDLE;
        }
        if (*mems[i] != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, *mems[i], nullptr);
            *mems[i] = VK_NULL_HANDLE;
        }
    }

    m_tileMapWidth       = 0;
    m_tileMapHeight      = 0;
    m_tileResourcesReady = false;
    m_tileUploadPending  = false;
}

// --- Dear ImGui -------------------------------------------------------------

bool VulkanRenderer::InitImGui(Window& window)
{
    if (m_imguiInitialized) {
        return true;
    }
    if (m_device == VK_NULL_HANDLE || m_renderPass == VK_NULL_HANDLE) {
        LOG_ERROR("[Vulkan] InitImGui called before the renderer is initialized");
        return false;
    }

    if (!ImGui_ImplSDL3_InitForVulkan(window.GetSDLWindow())) {
        LOG_ERROR("[Vulkan] ImGui_ImplSDL3_InitForVulkan failed");
        return false;
    }

    // The vendored backend allocates SAMPLED_IMAGE sets per texture plus a few
    // SAMPLER sets, and frees them individually, hence FREE_DESCRIPTOR_SET.
    // COMBINED_IMAGE_SAMPLER is included for the obsolete AddTexture overload.
    const VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 8 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 },
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 88;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_imguiDescriptorPool) !=
        VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateDescriptorPool failed for ImGui");
        m_imguiDescriptorPool = VK_NULL_HANDLE;
        ImGui_ImplSDL3_Shutdown();
        return false;
    }

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion     = VK_API_VERSION_1_2;  // matches VkApplicationInfo::apiVersion
    initInfo.Instance       = m_instance;
    initInfo.PhysicalDevice = m_physicalDevice;
    initInfo.Device         = m_device;
    initInfo.QueueFamily    = m_graphicsQueueFamily;
    initInfo.Queue          = m_graphicsQueue;
    initInfo.DescriptorPool = m_imguiDescriptorPool;
    initInfo.MinImageCount  = 2;
    initInfo.ImageCount     =
        std::max(static_cast<uint32_t>(m_swapchainImages.size()), 2u);
    initInfo.PipelineInfoMain.RenderPass  = m_renderPass;
    initInfo.PipelineInfoMain.Subpass     = 0;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        LOG_ERROR("[Vulkan] ImGui_ImplVulkan_Init failed");
        ImGui_ImplSDL3_Shutdown();
        vkDestroyDescriptorPool(m_device, m_imguiDescriptorPool, nullptr);
        m_imguiDescriptorPool = VK_NULL_HANDLE;
        return false;
    }

    m_imguiInitialized = true;
    LOG_INFO("[Vulkan] ImGui initialized");
    return true;
}

void VulkanRenderer::BeginImGuiFrame()
{
    if (!m_imguiInitialized) {
        return;
    }
    ImGui_ImplVulkan_NewFrame();
}

void VulkanRenderer::RenderImGui()
{
    // Skipped frames record no command buffer, so there is nothing to draw into.
    if (!m_imguiInitialized || !m_frameActive) {
        return;
    }
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr) {
        return;
    }
    ImGui_ImplVulkan_RenderDrawData(drawData, m_commandBuffers[m_frameIndex]);
}

void VulkanRenderer::ShutdownImGui()
{
    if (!m_imguiInitialized) {
        return;
    }
    m_imguiInitialized = false;

    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }
    // The application owns the ImGui context; skip the backend shutdowns if it
    // already destroyed it (they would assert on the missing context).
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
    }
    if (m_device != VK_NULL_HANDLE && m_imguiDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_imguiDescriptorPool, nullptr);
        m_imguiDescriptorPool = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::OnResize(int pixelWidth, int pixelHeight)
{
    // Only flagged here: tearing the swapchain down inside the event handler
    // would free objects the in-flight frames still reference. BeginFrame
    // rebuilds it at a point where that is safe.
    (void)pixelWidth;
    (void)pixelHeight;
    m_needsRecreate = true;
}

bool VulkanRenderer::RecreateSwapchain()
{
    int width  = 0;
    int height = 0;
    m_window->GetPixelSize(width, height);
    if (width <= 0 || height <= 0) {
        return false;  // minimized: keep the flag set and retry next frame
    }

    vkDeviceWaitIdle(m_device);
    DestroySwapchainObjects();

    // The render pass outlives this: the surface format does not change, only
    // the extent and the image count do. ImGui needs no notification either:
    // its pipeline hangs off that same render pass, and the MinImageCount it
    // was initialized with (2) never changes, so there is no
    // ImGui_ImplVulkan_SetMinImageCount to make.
    if (!CreateSwapchain() || !CreateImageViews() || !CreateFramebuffers() ||
        !CreateImageSemaphores()) {
        DestroySwapchainObjects();  // leave a clean null state, not a half-built one
        return false;
    }

    m_needsRecreate = false;
    return true;
}

void VulkanRenderer::DestroySwapchainObjects()
{
    if (m_device == VK_NULL_HANDLE) {
        return;
    }

    for (VkSemaphore semaphore : m_renderFinishedSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, semaphore, nullptr);
        }
    }
    m_renderFinishedSemaphores.clear();

    for (VkFramebuffer framebuffer : m_framebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
    }
    m_framebuffers.clear();

    for (VkImageView imageView : m_swapchainImageViews) {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, imageView, nullptr);
        }
    }
    m_swapchainImageViews.clear();

    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_swapchainImages.clear();  // owned by the swapchain, never destroyed directly
}

void VulkanRenderer::Shutdown()
{
    // Defensive: the application normally calls this itself before tearing
    // down its ImGui context. A no-op when it did.
    ShutdownImGui();

    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            if (m_imageAvailableSemaphores[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
                m_imageAvailableSemaphores[i] = VK_NULL_HANDLE;
            }
            if (m_inFlightFences[i] != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
                m_inFlightFences[i] = VK_NULL_HANDLE;
            }
        }

        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);  // frees its buffers too
            m_commandPool = VK_NULL_HANDLE;
        }
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            m_commandBuffers[i] = VK_NULL_HANDLE;
        }

        if (m_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
            m_pipeline = VK_NULL_HANDLE;
        }
        if (m_pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            m_pipelineLayout = VK_NULL_HANDLE;
        }

        DestroyTileImages();  // per-map images and the staging mirror

        if (m_tilePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_tilePipeline, nullptr);
            m_tilePipeline = VK_NULL_HANDLE;
        }
        if (m_tilePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, m_tilePipelineLayout, nullptr);
            m_tilePipelineLayout = VK_NULL_HANDLE;
        }
        if (m_tileDescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_tileDescriptorPool, nullptr);  // frees the set
            m_tileDescriptorPool = VK_NULL_HANDLE;
            m_tileDescriptorSet  = VK_NULL_HANDLE;
        }
        if (m_tileSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_tileSetLayout, nullptr);
            m_tileSetLayout = VK_NULL_HANDLE;
        }
        if (m_tileSampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device, m_tileSampler, nullptr);
            m_tileSampler = VK_NULL_HANDLE;
        }

        DestroySwapchainObjects();  // framebuffers reference the render pass

        if (m_renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            m_renderPass = VK_NULL_HANDLE;
        }

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

    m_physicalDevice  = VK_NULL_HANDLE;
    m_swapchainFormat = VK_FORMAT_UNDEFINED;
    m_extent          = VkExtent2D{};
    m_window          = nullptr;
    m_frameIndex      = 0;
    m_imageIndex      = 0;
    m_frameActive     = false;
    m_needsRecreate   = false;
}

} // namespace engine
