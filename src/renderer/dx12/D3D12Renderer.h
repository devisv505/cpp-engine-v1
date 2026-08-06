#pragma once

// windows.h first so COM basics (IID_PPV_ARGS et al.) are guaranteed present
// for the D3D headers. NOMINMAX / WIN32_LEAN_AND_MEAN come from the build.
#include <windows.h>

#include <cstdint>
#include <vector>

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

    bool CreateTileResources(const TileRenderData& data, int mapWidth, int mapHeight,
                             const uint16_t* tiles) override;
    void UpdateTileRegion(int x, int y, int w, int h, const uint16_t* tiles,
                          int mapWidth) override;
    void DrawTileMap(const TileDrawConstants& constants) override;

    void SetOccluders(const Wall* walls, int wallCount, float originX, float originY,
                      float worldWidth, float worldHeight) override;
    void DrawWalls(const Wall* walls, int wallCount) override;
    void DrawLighting(const Light* lights, int lightCount,
                      const TileDrawConstants& camera) override;

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

    // RTV heap: one descriptor per back buffer, plus one past them for the
    // cached world-space occlusion mask.
    static constexpr UINT kRtvHeapSize  = kFrameCount + 1;
    static constexpr UINT kMaskRtvIndex = kFrameCount;

    // Shader-visible SRV heap: slots 0-2 hold the tile textures (ids, atlas,
    // palette), slot 3 the occlusion mask. Sized with headroom for future
    // descriptors.
    static constexpr UINT kSrvHeapSize  = 64;
    static constexpr UINT kTileSrvCount = 3;
    static constexpr UINT kMaskSrvSlot  = 3;

    // Occlusion mask resolution: one texel per four world pixels, capped so a
    // huge world cannot ask for an unreasonable render target.
    static constexpr float kMaskWorldPxPerTexel = 4.0f;
    static constexpr UINT  kMaskMaxTexels       = 2048;

    // What the command list currently has bound. Draw calls bind lazily so
    // the tile, quad and light passes can interleave in any order.
    enum class BoundPipeline { None, Quad, Tile, Light };

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
    bool CreateRenderTargetTexture(UINT width, UINT height, DXGI_FORMAT format,
                                   Microsoft::WRL::ComPtr<ID3D12Resource>& texture);
    bool CreateUploadBuffer(UINT64 sizeBytes, Microsoft::WRL::ComPtr<ID3D12Resource>& buffer);
    void ReleaseTileTextures();
    void ReleaseTileUploadBuffers();
    void FlushPendingTileUpload();
    void BindQuadPipeline();
    void BindTilePipeline();

    // Volumetric lighting. The occluder objects serve the cached mask build,
    bool CreateOccluderRootSignature();
    bool CreateOccluderPipelineState();
    bool CreateLightRootSignature();
    bool CreateLightPipelineState();
    bool EnsureOccluderPipeline();
    bool EnsureLightingPipelines();
    void ReleaseOcclusionMask();
    void BindLightPipeline();

    void ReleaseRenderTargets();
    void ApplyPendingResize();
    void SetFramebufferSize(UINT width, UINT height);

    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle(UINT slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(UINT slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(UINT slot) const;


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

    // Volumetric lighting. The occlusion mask is a world-space R8 texture
    // rebuilt only by SetOccluders; the light pass owns one root
    // signature and differ only in pipeline state (additive vs alpha blend).
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_occluderRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_occluderPipelineState;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_lightRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_lightPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_occlusionMask;

    // World rectangle the mask covers, in world pixels, and its resolution.
    // The light shader needs both to map a world position to a mask UV.
    float m_maskOriginX = 0.0f;
    float m_maskOriginY = 0.0f;
    float m_maskWorldW  = 0.0f;
    float m_maskWorldH  = 0.0f;
    UINT  m_maskWidth   = 0;
    UINT  m_maskHeight  = 0;

    // Camera of the last DrawTileMap: DrawWalls gets no camera of its own.
    TileDrawConstants m_lastCamera{};

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

    bool m_vsync               = true;
    bool m_tearingSupported    = false;
    bool m_resizePending       = false;
    bool m_frameActive         = false;  // BeginFrame succeeded and EndFrame has not run
    bool m_tileResourcesReady  = false;
    bool m_cameraValid         = false;  // m_lastCamera holds a real camera
    bool m_maskReady           = false;  // mask texture, RTV and SRV are all live
    bool m_lightingUnavailable = false;  // shader compile failed once; do not retry
};

} // namespace engine
