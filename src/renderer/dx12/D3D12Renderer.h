#pragma once

// windows.h first so COM basics (IID_PPV_ARGS et al.) are guaranteed present
// for the D3D headers. NOMINMAX / WIN32_LEAN_AND_MEAN come from the build.
#include <windows.h>

#include <cstdint>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "core/events/EventBus.h"
#include "renderer/IRenderer.h"

namespace engine {

class D3D12Renderer final : public IRenderer {
public:
    ~D3D12Renderer() override;

    bool Init(Window& window, EventBus& events) override;
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


    const char* GetBackendName() const override { return "Direct3D 12"; }

private:
    // Double buffering: one command allocator and one fence value per back
    // buffer, so the CPU can record frame N+1 while the GPU still reads N.
    static constexpr UINT kFrameCount = 2;

    // Root constants are counted in DWORDs; both constant blocks are 64 bytes.
    static constexpr UINT kQuadConstantCount =
        static_cast<UINT>(sizeof(QuadConstants) / sizeof(uint32_t));
    static constexpr UINT kTileConstantCount =
        static_cast<UINT>(sizeof(TileDrawConstants) / sizeof(uint32_t));

    // Shader-visible SRV heap: slots 0-2 hold the tile textures (ids, atlas,
    // palette). Sized with headroom for future descriptors.
    static constexpr UINT kSrvHeapSize  = 64;
    static constexpr UINT kTileSrvCount = 3;

    // What the command list currently has bound. Draw calls bind lazily so
    // the tile and quad passes can interleave in any order.
    enum class BoundPipeline { None, Quad, Tile };

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

    bool CreateSrvHeap();
    bool CreateTileRootSignature();
    bool CreateTilePipelineState();
    bool CreateTexture2D(UINT width, UINT height, DXGI_FORMAT format,
                         Microsoft::WRL::ComPtr<ID3D12Resource>& texture);
    bool CreateUploadBuffer(UINT64 sizeBytes, Microsoft::WRL::ComPtr<ID3D12Resource>& buffer);
    void ReleaseTileTextures();
    void ReleaseTileUploadBuffers();
    void FlushPendingTileUpload();
    void BindQuadPipeline();
    void BindTilePipeline();

    void ReleaseRenderTargets();
    void ApplyPendingResize();
    void SetFramebufferSize(UINT width, UINT height);

    D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(UINT slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(UINT slot) const;


    // Blocks until the fence reaches `value`; WaitForGpu drains everything
    // submitted so far, which is what resize and shutdown need.
    void WaitForFenceValue(UINT64 value);
    void WaitForGpu();

    Subscription m_onWindowResized;

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

    // Tile map pass.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_tileIdTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_atlasTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_paletteTexture;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_tileRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_tilePipelineState;

    // Per frame in flight, sized for the whole map and persistently mapped,
    // so region updates never write a buffer the GPU may still be reading.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_tileUploadBuffers[kFrameCount];
    uint8_t*                               m_tileUploadMapped[kFrameCount] = {};

    std::vector<uint16_t> m_tileCpu;      // CPU copy of the full tile-id grid

    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;                 // last value signalled on the queue
    UINT64 m_fenceValues[kFrameCount] = {};  // per-frame value of its last submission

    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT     m_scissor{};

    UINT m_frameIndex        = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT m_srvDescriptorSize = 0;
    UINT m_swapChainFlags    = 0;
    UINT m_width             = 0;
    UINT m_height            = 0;

    int m_mapWidth  = 0;
    int m_mapHeight = 0;

    // Dirty tile rectangle accumulated by UpdateTileRegion, flushed onto the
    // frame's command list at the start of the next BeginFrame.
    int  m_dirtyX0 = 0, m_dirtyY0 = 0;
    int  m_dirtyX1 = 0, m_dirtyY1 = 0;
    bool m_tileUpdatePending = false;

    // Resizes are recorded here and applied at a frame boundary, never in the
    // middle of a recorded command list.
    UINT m_pendingWidth  = 0;
    UINT m_pendingHeight = 0;

    BoundPipeline m_boundPipeline = BoundPipeline::None;

    bool m_vsync              = true;
    bool m_tearingSupported   = false;
    bool m_resizePending      = false;
    bool m_frameActive        = false;  // BeginFrame succeeded and EndFrame has not run
    bool m_tileResourcesReady = false;
};

} // namespace engine
