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
    // the extent and the image count do.
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
