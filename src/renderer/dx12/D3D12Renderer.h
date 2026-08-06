#pragma once

// windows.h first so COM basics (IID_PPV_ARGS et al.) are guaranteed present
// for the D3D headers. NOMINMAX / WIN32_LEAN_AND_MEAN come from the build.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "renderer/IRenderer.h"

namespace engine {

class D3D12Renderer final : public IRenderer {
public:
    ~D3D12Renderer() override;

    bool Init(Window& window) override;
    void Shutdown() override;
    const char* GetBackendName() const override { return "Direct3D 12"; }

private:
    bool CreateFactory();
    bool PickAdapter();
    bool CreateDevice();
    bool CreateCommandQueue();

    Microsoft::WRL::ComPtr<IDXGIFactory6>      m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>      m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device>       m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
};

} // namespace engine
