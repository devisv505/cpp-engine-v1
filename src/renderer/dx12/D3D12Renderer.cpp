#include "renderer/dx12/D3D12Renderer.h"

#include <string>

#include <d3d12sdklayers.h>

#include "core/Log.h"
#include "core/Window.h"

using Microsoft::WRL::ComPtr;

namespace engine {

namespace {

std::string ToUtf8(const wchar_t* wide)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return "(unknown adapter)";
    }
    std::string utf8(static_cast<size_t>(size) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

} // namespace

D3D12Renderer::~D3D12Renderer()
{
    Shutdown();
}

bool D3D12Renderer::Init(Window& window)
{
    // The window is not touched yet: D3D12 only needs the HWND once a
    // swapchain is created, which is future work.
    (void)window;

    if (!CreateFactory())      return false;
    if (!PickAdapter())        return false;
    if (!CreateDevice())       return false;
    if (!CreateCommandQueue()) return false;
    return true;
}

bool D3D12Renderer::CreateFactory()
{
    UINT dxgiFlags = 0;

#ifndef NDEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
        LOG_INFO("[D3D12] Debug layer enabled");
    }
#endif

    if (FAILED(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&m_factory)))) {
        LOG_ERROR("[D3D12] CreateDXGIFactory2 failed");
        return false;
    }
    return true;
}

bool D3D12Renderer::PickAdapter()
{
    for (UINT i = 0;
         m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                               IID_PPV_ARGS(&m_adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        m_adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;  // skip WARP / software adapters
        }
        // Probe support without creating the device yet.
        if (SUCCEEDED(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            LOG_INFO("[D3D12] Adapter: %s", ToUtf8(desc.Description).c_str());
            return true;
        }
    }

    m_adapter.Reset();
    LOG_ERROR("[D3D12] No hardware adapter supporting feature level 11_0 found");
    return false;
}

bool D3D12Renderer::CreateDevice()
{
    if (FAILED(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&m_device)))) {
        LOG_ERROR("[D3D12] D3D12CreateDevice failed");
        return false;
    }
    LOG_INFO("[D3D12] Device created (feature level 11_0)");
    return true;
}

bool D3D12Renderer::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    if (FAILED(m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_commandQueue)))) {
        LOG_ERROR("[D3D12] CreateCommandQueue failed");
        return false;
    }
    LOG_INFO("[D3D12] Direct command queue created");
    return true;
}

void D3D12Renderer::Shutdown()
{
    m_commandQueue.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_factory.Reset();
}

} // namespace engine
