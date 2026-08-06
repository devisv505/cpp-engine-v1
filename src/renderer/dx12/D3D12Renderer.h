#pragma once

// windows.h first so COM basics (IID_PPV_ARGS et al.) are guaranteed present
// for the D3D headers. NOMINMAX / WIN32_LEAN_AND_MEAN come from the build.
#include <windows.h>

#include <cstdint>

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
    void BeginFrame(const Color& clearColor) override;
    void DrawQuad(const Quad& quad) override;
    void EndFrame() override;
    void OnResize(int pixelWidth, int pixelHeight) override;
    const char* GetBackendName() const override { return "Direct3D 12"; }

private:
    // Double buffering: one command allocator and one fence value per back
    // buffer, so the CPU can record frame N+1 while the GPU still reads N.
    static constexpr UINT kFrameCount = 2;

    // QuadConstants travels as root constants, which are counted in DWORDs.
    static constexpr UINT kQuadConstantCount =
        static_cast<UINT>(sizeof(QuadConstants) / sizeof(uint32_t));

    bool CreateFactory();
    bool PickAdapter();
    bool CreateDevice();
    bool CreateCommandQueue();
    bool CreateSwapChain(Window& window);
    bool CreateRenderTargets();
    bool CreateRootSignature();
    bool CreatePipelineState();
    bool CreateCommandObjects();
    bool CreateSyncObjects();

    void ReleaseRenderTargets();
    void ApplyPendingResize();
    void SetFramebufferSize(UINT width, UINT height);

    // Blocks until the fence reaches `value`; WaitForGpu drains everything
    // submitted so far, which is what resize and shutdown need.
    void WaitForFenceValue(UINT64 value);
    void WaitForGpu();

    Microsoft::WRL::ComPtr<IDXGIFactory6>             m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>             m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device>              m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        m_commandQueue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3>           m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_renderTargets[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12RootSignature>       m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>       m_pipelineState;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_commandAllocators[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence>               m_fence;

    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;                 // last value signalled on the queue
    UINT64 m_fenceValues[kFrameCount] = {};  // per-frame value of its last submission

    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT     m_scissor{};

    UINT m_frameIndex        = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT m_swapChainFlags    = 0;
    UINT m_width             = 0;
    UINT m_height            = 0;

    // Resizes are recorded here and applied at a frame boundary, never in the
    // middle of a recorded command list.
    UINT m_pendingWidth  = 0;
    UINT m_pendingHeight = 0;

    bool m_vsync            = true;
    bool m_tearingSupported = false;
    bool m_resizePending    = false;
    bool m_frameActive      = false;  // BeginFrame succeeded and EndFrame has not run
};

} // namespace engine
