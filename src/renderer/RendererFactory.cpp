#include "renderer/RendererFactory.h"

#if defined(ENGINE_BACKEND_D3D12)
  #include "renderer/dx12/D3D12Renderer.h"
#elif defined(ENGINE_BACKEND_VULKAN)
  #include "renderer/vulkan/VulkanRenderer.h"
#elif defined(ENGINE_BACKEND_METAL)
  #include "renderer/metal/MetalRenderer.h"
#else
  #error "No renderer backend selected (the build must define ENGINE_BACKEND_D3D12/VULKAN/METAL)"
#endif

namespace engine {

RendererBackend GetActiveBackend()
{
#if defined(ENGINE_BACKEND_D3D12)
    return RendererBackend::D3D12;
#elif defined(ENGINE_BACKEND_VULKAN)
    return RendererBackend::Vulkan;
#elif defined(ENGINE_BACKEND_METAL)
    return RendererBackend::Metal;
#endif
}

SDL_WindowFlags GetRequiredWindowFlags()
{
#if defined(ENGINE_BACKEND_D3D12)
    // D3D12 needs no creation flag; the HWND is consumed at swapchain time.
    return 0;
#elif defined(ENGINE_BACKEND_VULKAN)
    return SDL_WINDOW_VULKAN;
#elif defined(ENGINE_BACKEND_METAL)
    return SDL_WINDOW_METAL;
#endif
}

std::unique_ptr<IRenderer> CreateRenderer()
{
#if defined(ENGINE_BACKEND_D3D12)
    return std::make_unique<D3D12Renderer>();
#elif defined(ENGINE_BACKEND_VULKAN)
    return std::make_unique<VulkanRenderer>();
#elif defined(ENGINE_BACKEND_METAL)
    return std::make_unique<MetalRenderer>();
#endif
}

} // namespace engine
