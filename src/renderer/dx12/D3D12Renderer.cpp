#include "renderer/dx12/D3D12Renderer.h"

#include <climits>
#include <cstring>
#include <string>

#include <d3d12sdklayers.h>
#include <d3dcompiler.h>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>


#include "core/Log.h"
#include "core/Window.h"
#include "world/TileAtlas.h"
#include "world/TileRegistry.h"

using Microsoft::WRL::ComPtr;

namespace engine {

namespace {

// One quad per draw, no vertex buffers: the four corners come from SV_VertexID
// and the root constants, which mirror QuadConstants field for field.
constexpr char kQuadShaderSource[] = R"(
cbuffer QuadConstants : register(b0)
{
    float4 rect;      // x, y, w, h in pixels
    float4 color;     // rgba
    float2 viewport;  // framebuffer size in pixels
    float2 padding;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color    : COLOR0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 corner = float2(float(vertexId & 1u), float((vertexId >> 1u) & 1u));
    float2 pixel  = rect.xy + corner * rect.zw;

    VSOutput output;
    // Pixel space is top-left origin with +Y down; D3D12 clip space is Y-up.
    output.position = float4(pixel.x / viewport.x * 2.0 - 1.0,
                             1.0 - pixel.y / viewport.y * 2.0,
                             0.0,
                             1.0);
    output.color = color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return input.color;
}
)";

// Fullscreen tile pass: each fragment finds the world tile it covers from the
// camera constants, reads the tile id (t0), and resolves the final color
// through the palette (t2) and atlas (t1). The cbuffer mirrors
// TileDrawConstants field for field.
constexpr char kTileShaderSource[] = R"(
cbuffer TileConstants : register(b0)
{
    float2 camera;      // world pixels at the viewport center
    float  zoom;        // screen pixels per world pixel
    float  tileSizePx;  // world pixels per tile
    float2 viewport;    // framebuffer size in pixels
    float2 mapSize;     // map size in tiles
    float4 background;  // color outside the map bounds
    float4 padding;
};

Texture2D<uint>   tileIds    : register(t0);
Texture2D         atlas      : register(t1);
Texture2D<float4> palette    : register(t2);
SamplerState      pointClamp : register(s0);

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    // One triangle that covers the whole viewport.
    const float2 corners[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };

    VSOutput output;
    output.position = float4(corners[vertexId], 0.0, 1.0);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    // SV_Position arrives as top-left-origin pixel coordinates with +Y down,
    // which matches world space, so there are no axis flips anywhere.
    float2 worldPx = camera + (input.position.xy - viewport * 0.5) / zoom;
    float2 tilePos = floor(worldPx / tileSizePx);

    if (any(tilePos < 0.0) || any(tilePos >= mapSize)) {
        return background;
    }

    uint   id       = min(tileIds.Load(int3(int2(tilePos), 0)), 255u);
    float4 colorRow = palette.Load(int3(int(id), 0, 0));  // rgb + has-texture flag
    float4 uvRow    = palette.Load(int3(int(id), 1, 0));  // atlas UV rect

    float3 result = colorRow.rgb;
    if (colorRow.a > 0.5) {
        float2 f  = frac(worldPx / tileSizePx);
        float2 uv = lerp(uvRow.xy, uvRow.zw, f);
        result = atlas.Sample(pointClamp, uv).rgb * colorRow.rgb;
    }
    return float4(result, 1.0);
}
)";

// The tile cbuffer above is written against this exact size; a change to
// TileDrawConstants must update the shader too.
static_assert(sizeof(TileDrawConstants) == 16 * sizeof(uint32_t),
              "TileDrawConstants must stay 16 root constants");

constexpr UINT AlignUp(UINT value, UINT alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

constexpr UINT64 AlignUp64(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

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

bool CompileShader(const char* source, size_t sourceSize, const char* sourceName,
                   const char* entryPoint, const char* target, ComPtr<ID3DBlob>& blob)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, sourceSize, sourceName, nullptr, nullptr, entryPoint,
                                  target, flags, 0, &blob, &errors);
    if (FAILED(hr)) {
        LOG_ERROR("[D3D12] %s %s compilation failed (0x%08X): %s", sourceName, target,
                  static_cast<unsigned>(hr),
                  errors ? static_cast<const char*>(errors->GetBufferPointer())
                         : "(no diagnostics)");
        return false;
    }
    return true;
}

} // namespace

D3D12Renderer::~D3D12Renderer()
{
    Shutdown();
}

bool D3D12Renderer::Init(Window& window)
{
    if (!CreateFactory())         return false;
    if (!PickAdapter())           return false;
    if (!CreateDevice())          return false;
    if (!CreateCommandQueue())    return false;
    if (!CreateSwapChain(window)) return false;
    if (!CreateRenderTargets())   return false;
    if (!CreateRootSignature())   return false;
    if (!CreatePipelineState())   return false;
    if (!CreateCommandObjects())  return false;
    if (!CreateSyncObjects())     return false;
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

bool D3D12Renderer::CreateSwapChain(Window& window)
{
    // SDL3 exposes native handles through window properties; the old syswm API
    // is gone.
    HWND hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window.GetSDLWindow()),
                               SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) {
        LOG_ERROR("[D3D12] Window has no Win32 HWND property");
        return false;
    }

    int pixelWidth  = 0;
    int pixelHeight = 0;
    window.GetPixelSize(pixelWidth, pixelHeight);

    const UINT width  = pixelWidth  > 0 ? static_cast<UINT>(pixelWidth)  : 1;
    const UINT height = pixelHeight > 0 ? static_cast<UINT>(pixelHeight) : 1;

    m_vsync = window.GetConfig().vsync;

    // Tearing only matters with vsync off, and the flag has to be set both at
    // creation time and on every Present.
    if (!m_vsync) {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                     &allowTearing, sizeof(allowTearing))) &&
            allowTearing) {
            m_tearingSupported = true;
            m_swapChainFlags   = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width              = width;
    desc.Height             = height;
    desc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Stereo             = FALSE;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount        = kFrameCount;
    desc.Scaling            = DXGI_SCALING_STRETCH;
    desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode          = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags              = m_swapChainFlags;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(m_factory->CreateSwapChainForHwnd(m_commandQueue.Get(), hwnd, &desc, nullptr,
                                                 nullptr, &swapChain1))) {
        LOG_ERROR("[D3D12] CreateSwapChainForHwnd failed");
        return false;
    }

    // DXGI's own Alt+Enter handling would fight SDL's fullscreen management.
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    if (FAILED(swapChain1.As(&m_swapChain))) {
        LOG_ERROR("[D3D12] IDXGISwapChain3 is not available");
        return false;
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    SetFramebufferSize(width, height);

    LOG_INFO("[D3D12] Swapchain created: %ux%u, %u buffers, vsync %s%s", width, height, kFrameCount,
             m_vsync ? "on" : "off", m_tearingSupported ? ", tearing allowed" : "");
    return true;
}

bool D3D12Renderer::CreateRenderTargets()
{
    if (!m_rtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = kFrameCount;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask       = 0;

        if (FAILED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap)))) {
            LOG_ERROR("[D3D12] CreateDescriptorHeap(RTV) failed");
            return false;
        }
        m_rtvDescriptorSize =
            m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])))) {
            LOG_ERROR("[D3D12] IDXGISwapChain::GetBuffer(%u) failed", i);
            return false;
        }
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, handle);
        handle.ptr += static_cast<SIZE_T>(m_rtvDescriptorSize);
    }
    return true;
}

bool D3D12Renderer::CreateRootSignature()
{
    // The whole per-draw state is one block of root constants at b0: no
    // descriptor tables, and no vertex buffers, so no input assembler flag.
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.RegisterSpace  = 0;
    param.Constants.Num32BitValues = kQuadConstantCount;
    param.ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = 1;
    desc.pParameters       = &param;
    desc.NumStaticSamplers = 0;
    desc.pStaticSamplers   = nullptr;
    desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                           &errors))) {
        LOG_ERROR("[D3D12] Root signature serialization failed: %s",
                  errors ? static_cast<const char*>(errors->GetBufferPointer())
                         : "(no diagnostics)");
        return false;
    }

    if (FAILED(m_device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                             serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&m_rootSignature)))) {
        LOG_ERROR("[D3D12] CreateRootSignature failed");
        return false;
    }
    return true;
}

bool D3D12Renderer::CreatePipelineState()
{
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    if (!CompileShader(kQuadShaderSource, sizeof(kQuadShaderSource) - 1, "quad.hlsl", "VSMain",
                       "vs_5_1", vertexShader)) {
        return false;
    }
    if (!CompileShader(kQuadShaderSource, sizeof(kQuadShaderSource) - 1, "quad.hlsl", "PSMain",
                       "ps_5_1", pixelShader)) {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_rootSignature.Get();

    desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
    desc.VS.BytecodeLength  = vertexShader->GetBufferSize();
    desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
    desc.PS.BytecodeLength  = pixelShader->GetBufferSize();

    desc.BlendState.AlphaToCoverageEnable  = FALSE;
    desc.BlendState.IndependentBlendEnable = FALSE;

    D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
    blend.BlendEnable           = TRUE;
    blend.LogicOpEnable         = FALSE;
    blend.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp               = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    blend.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    blend.LogicOp               = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    desc.SampleMask = UINT_MAX;

    desc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthBias             = D3D12_DEFAULT_DEPTH_BIAS;
    desc.RasterizerState.DepthBiasClamp        = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.RasterizerState.SlopeScaledDepthBias  = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.RasterizerState.DepthClipEnable       = TRUE;
    desc.RasterizerState.MultisampleEnable     = FALSE;
    desc.RasterizerState.AntialiasedLineEnable = FALSE;
    desc.RasterizerState.ForcedSampleCount     = 0;
    desc.RasterizerState.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    desc.DepthStencilState.DepthEnable   = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;

    desc.InputLayout.pInputElementDescs = nullptr;
    desc.InputLayout.NumElements        = 0;
    desc.PrimitiveTopologyType          = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets               = 1;
    desc.RTVFormats[0]                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat                      = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count               = 1;
    desc.SampleDesc.Quality             = 0;
    desc.NodeMask                       = 0;
    desc.Flags                          = D3D12_PIPELINE_STATE_FLAG_NONE;

    if (FAILED(m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pipelineState)))) {
        LOG_ERROR("[D3D12] CreateGraphicsPipelineState failed");
        return false;
    }
    return true;
}

bool D3D12Renderer::CreateCommandObjects()
{
    for (UINT i = 0; i < kFrameCount; ++i) {
        if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&m_commandAllocators[i])))) {
            LOG_ERROR("[D3D12] CreateCommandAllocator(%u) failed", i);
            return false;
        }
    }

    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           m_commandAllocators[m_frameIndex].Get(),
                                           m_pipelineState.Get(),
                                           IID_PPV_ARGS(&m_commandList)))) {
        LOG_ERROR("[D3D12] CreateCommandList failed");
        return false;
    }

    // Command lists are created open; BeginFrame expects a closed one.
    if (FAILED(m_commandList->Close())) {
        LOG_ERROR("[D3D12] Initial command list Close failed");
        return false;
    }
    return true;
}

bool D3D12Renderer::CreateSyncObjects()
{
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
        LOG_ERROR("[D3D12] CreateFence failed");
        return false;
    }

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        LOG_ERROR("[D3D12] CreateEvent failed (0x%08X)", static_cast<unsigned>(GetLastError()));
        return false;
    }
    return true;
}

void D3D12Renderer::BeginFrame(const Color& clearColor)
{
    m_frameActive = false;
    if (!m_swapChain || !m_commandList) {
        return;
    }

    if (m_resizePending) {
        ApplyPendingResize();
    }
    if (m_width == 0 || m_height == 0 || !m_renderTargets[m_frameIndex]) {
        return;
    }

    WaitForFenceValue(m_fenceValues[m_frameIndex]);

    if (FAILED(m_commandAllocators[m_frameIndex]->Reset())) {
        LOG_ERROR("[D3D12] Command allocator reset failed");
        return;
    }
    // Pipelines are bound lazily per draw path (quad / tile), so the
    // list starts without one.
    if (FAILED(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr))) {
        LOG_ERROR("[D3D12] Command list reset failed");
        return;
    }

    // This frame's fence has been waited on, so its upload buffer is idle and
    // pending tile edits can be copied before any draw reads the texture.
    FlushPendingTileUpload();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(m_frameIndex) * static_cast<SIZE_T>(m_rtvDescriptorSize);
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float clear[4] = {clearColor.r, clearColor.g, clearColor.b, clearColor.a};
    m_commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissor);

    m_boundPipeline = BoundPipeline::None;
    m_frameActive   = true;
}

void D3D12Renderer::BindQuadPipeline()
{
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetPipelineState(m_pipelineState.Get());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_boundPipeline = BoundPipeline::Quad;
}

void D3D12Renderer::DrawQuad(const Quad& quad)
{
    if (!m_frameActive) {
        return;
    }
    if (m_boundPipeline != BoundPipeline::Quad) {
        BindQuadPipeline();
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
    constants.viewport[0] = static_cast<float>(m_width);
    constants.viewport[1] = static_cast<float>(m_height);
    constants.padding[0]  = 0.0f;
    constants.padding[1]  = 0.0f;

    m_commandList->SetGraphicsRoot32BitConstants(0, kQuadConstantCount, &constants, 0);
    m_commandList->DrawInstanced(4, 1, 0, 0);  // triangle strip over the four corners
}

void D3D12Renderer::EndFrame()
{
    if (!m_frameActive) {
        return;
    }
    m_frameActive = false;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);

    if (FAILED(m_commandList->Close())) {
        LOG_ERROR("[D3D12] Command list Close failed; frame dropped");
        return;
    }

    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);

    const UINT syncInterval = m_vsync ? 1 : 0;
    const UINT presentFlags = (!m_vsync && m_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;

    const HRESULT hr = m_swapChain->Present(syncInterval, presentFlags);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            LOG_ERROR("[D3D12] Device removed during Present (reason 0x%08X)",
                      static_cast<unsigned>(m_device->GetDeviceRemovedReason()));
        } else {
            LOG_ERROR("[D3D12] Present failed (0x%08X)", static_cast<unsigned>(hr));
        }
    }

    // Record when this frame's work is done so the next use of the same
    // allocator can wait for exactly that submission.
    const UINT64 value = ++m_fenceValue;
    if (SUCCEEDED(m_commandQueue->Signal(m_fence.Get(), value))) {
        m_fenceValues[m_frameIndex] = value;
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12Renderer::OnResize(int pixelWidth, int pixelHeight)
{
    if (pixelWidth <= 0 || pixelHeight <= 0) {
        return;  // minimized: keep the current swapchain until it comes back
    }

    const UINT width  = static_cast<UINT>(pixelWidth);
    const UINT height = static_cast<UINT>(pixelHeight);
    if (width == m_width && height == m_height) {
        return;
    }

    m_pendingWidth  = width;
    m_pendingHeight = height;
    m_resizePending = true;
}

void D3D12Renderer::ApplyPendingResize()
{
    m_resizePending = false;

    const UINT width  = m_pendingWidth;
    const UINT height = m_pendingHeight;
    if (width == 0 || height == 0 || (width == m_width && height == m_height)) {
        return;
    }

    // The back buffers must have no outstanding GPU work and no live
    // references before ResizeBuffers will touch them.
    WaitForGpu();
    ReleaseRenderTargets();

    const HRESULT hr =
        m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, m_swapChainFlags);
    if (FAILED(hr)) {
        LOG_ERROR("[D3D12] ResizeBuffers failed (0x%08X)", static_cast<unsigned>(hr));
    } else {
        SetFramebufferSize(width, height);
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (!CreateRenderTargets()) {
        LOG_ERROR("[D3D12] Render targets could not be recreated; frames will be skipped");
    }
}

void D3D12Renderer::SetFramebufferSize(UINT width, UINT height)
{
    m_width  = width;
    m_height = height;

    m_viewport.TopLeftX = 0.0f;
    m_viewport.TopLeftY = 0.0f;
    m_viewport.Width    = static_cast<float>(width);
    m_viewport.Height   = static_cast<float>(height);
    m_viewport.MinDepth = D3D12_MIN_DEPTH;
    m_viewport.MaxDepth = D3D12_MAX_DEPTH;

    m_scissor.left   = 0;
    m_scissor.top    = 0;
    m_scissor.right  = static_cast<LONG>(width);
    m_scissor.bottom = static_cast<LONG>(height);
}

void D3D12Renderer::WaitForFenceValue(UINT64 value)
{
    if (!m_fence || !m_fenceEvent || m_fence->GetCompletedValue() >= value) {
        return;
    }
    if (FAILED(m_fence->SetEventOnCompletion(value, m_fenceEvent))) {
        LOG_ERROR("[D3D12] SetEventOnCompletion failed");
        return;
    }
    WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
}

void D3D12Renderer::WaitForGpu()
{
    if (!m_commandQueue || !m_fence || !m_fenceEvent) {
        return;
    }

    const UINT64 value = ++m_fenceValue;
    if (FAILED(m_commandQueue->Signal(m_fence.Get(), value))) {
        return;
    }
    WaitForFenceValue(value);
}

void D3D12Renderer::ReleaseRenderTargets()
{
    for (UINT i = 0; i < kFrameCount; ++i) {
        m_renderTargets[i].Reset();
    }
}

// --- Tile map ---------------------------------------------------------------

bool D3D12Renderer::CreateSrvHeap()
{
    if (m_srvHeap) {
        return true;
    }

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kSrvHeapSize;
    desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NodeMask       = 0;

    if (FAILED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_srvHeap)))) {
        LOG_ERROR("[D3D12] CreateDescriptorHeap(SRV) failed");
        return false;
    }
    m_srvDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::SrvCpuHandle(UINT slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * static_cast<SIZE_T>(m_srvDescriptorSize);
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::SrvGpuHandle(UINT slot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(slot) * static_cast<UINT64>(m_srvDescriptorSize);
    return handle;
}

bool D3D12Renderer::CreateTexture2D(UINT width, UINT height, DXGI_FORMAT format,
                                    ComPtr<ID3D12Resource>& texture)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type                 = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask     = 0;
    heap.VisibleNodeMask      = 0;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment          = 0;
    desc.Width              = width;
    desc.Height             = height;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = format;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&texture)))) {
        LOG_ERROR("[D3D12] CreateCommittedResource(texture %ux%u) failed", width, height);
        return false;
    }
    return true;
}

bool D3D12Renderer::CreateUploadBuffer(UINT64 sizeBytes, ComPtr<ID3D12Resource>& buffer)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type                 = D3D12_HEAP_TYPE_UPLOAD;
    heap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask     = 0;
    heap.VisibleNodeMask      = 0;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment          = 0;
    desc.Width              = sizeBytes;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&buffer)))) {
        LOG_ERROR("[D3D12] CreateCommittedResource(upload buffer, %llu bytes) failed",
                  static_cast<unsigned long long>(sizeBytes));
        return false;
    }
    return true;
}

bool D3D12Renderer::CreateTileRootSignature()
{
    // Param 0: TileDrawConstants as root constants at b0. Param 1: the three
    // tile textures (t0-t2) as one descriptor table, plus a point-clamp
    // static sampler at s0.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors                    = kTileSrvCount;
    range.BaseShaderRegister                = 0;
    range.RegisterSpace                     = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace  = 0;
    params[0].Constants.Num32BitValues = kTileConstantCount;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &range;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias       = 0.0f;
    sampler.MaxAnisotropy    = 1;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD           = 0.0f;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = 2;
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &sampler;
    desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                           &errors))) {
        LOG_ERROR("[D3D12] Tile root signature serialization failed: %s",
                  errors ? static_cast<const char*>(errors->GetBufferPointer())
                         : "(no diagnostics)");
        return false;
    }

    if (FAILED(m_device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                             serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&m_tileRootSignature)))) {
        LOG_ERROR("[D3D12] Tile CreateRootSignature failed");
        return false;
    }
    return true;
}

bool D3D12Renderer::CreateTilePipelineState()
{
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    if (!CompileShader(kTileShaderSource, sizeof(kTileShaderSource) - 1, "tile.hlsl", "VSMain",
                       "vs_5_1", vertexShader)) {
        return false;
    }
    if (!CompileShader(kTileShaderSource, sizeof(kTileShaderSource) - 1, "tile.hlsl", "PSMain",
                       "ps_5_1", pixelShader)) {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_tileRootSignature.Get();

    desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
    desc.VS.BytecodeLength  = vertexShader->GetBufferSize();
    desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
    desc.PS.BytecodeLength  = pixelShader->GetBufferSize();

    desc.BlendState.AlphaToCoverageEnable  = FALSE;
    desc.BlendState.IndependentBlendEnable = FALSE;

    // Opaque fullscreen pass: every pixel is written, so no blending.
    D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
    blend.BlendEnable           = FALSE;
    blend.LogicOpEnable         = FALSE;
    blend.SrcBlend              = D3D12_BLEND_ONE;
    blend.DestBlend             = D3D12_BLEND_ZERO;
    blend.BlendOp               = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    blend.DestBlendAlpha        = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    blend.LogicOp               = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    desc.SampleMask = UINT_MAX;

    desc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthBias             = D3D12_DEFAULT_DEPTH_BIAS;
    desc.RasterizerState.DepthBiasClamp        = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.RasterizerState.SlopeScaledDepthBias  = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.RasterizerState.DepthClipEnable       = TRUE;
    desc.RasterizerState.MultisampleEnable     = FALSE;
    desc.RasterizerState.AntialiasedLineEnable = FALSE;
    desc.RasterizerState.ForcedSampleCount     = 0;
    desc.RasterizerState.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    desc.DepthStencilState.DepthEnable   = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;

    desc.InputLayout.pInputElementDescs = nullptr;
    desc.InputLayout.NumElements        = 0;
    desc.PrimitiveTopologyType          = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets               = 1;
    desc.RTVFormats[0]                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat                      = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count               = 1;
    desc.SampleDesc.Quality             = 0;
    desc.NodeMask                       = 0;
    desc.Flags                          = D3D12_PIPELINE_STATE_FLAG_NONE;

    if (FAILED(m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_tilePipelineState)))) {
        LOG_ERROR("[D3D12] Tile CreateGraphicsPipelineState failed");
        return false;
    }
    return true;
}

bool D3D12Renderer::CreateTileResources(const TileRenderData& data, int mapWidth, int mapHeight,
                                        const uint16_t* tiles)
{
    if (!m_device || !m_commandQueue || !m_commandList) {
        LOG_ERROR("[D3D12] CreateTileResources called before Init");
        return false;
    }
    if (m_frameActive) {
        LOG_ERROR("[D3D12] CreateTileResources must not run inside a frame");
        return false;
    }
    if (mapWidth <= 0 || mapHeight <= 0 || !tiles) {
        LOG_ERROR("[D3D12] CreateTileResources: invalid map (%dx%d)", mapWidth, mapHeight);
        return false;
    }
    if (data.atlasWidth <= 0 || data.atlasHeight <= 0 ||
        data.atlasPixels.size() < static_cast<size_t>(data.atlasWidth) *
                                      static_cast<size_t>(data.atlasHeight) * 4u) {
        LOG_ERROR("[D3D12] CreateTileResources: invalid atlas data");
        return false;
    }
    if (data.palettePixels.size() < static_cast<size_t>(TileRegistry::kMaxTileTypes) * 2u * 4u) {
        LOG_ERROR("[D3D12] CreateTileResources: invalid palette data");
        return false;
    }

    // Callable again after a map regenerate/resize: the old textures may still
    // be referenced by in-flight frames, so drain the GPU before replacing
    // them. Their SRV heap slots (0-2) are simply rewritten below.
    WaitForGpu();
    m_tileResourcesReady = false;
    m_tileUpdatePending  = false;
    ReleaseTileTextures();
    ReleaseTileUploadBuffers();

    if (!CreateSrvHeap()) {
        return false;
    }
    if (!m_tileRootSignature && !CreateTileRootSignature()) {
        return false;
    }
    if (!m_tilePipelineState && !CreateTilePipelineState()) {
        return false;
    }

    const UINT mapW     = static_cast<UINT>(mapWidth);
    const UINT mapH     = static_cast<UINT>(mapHeight);
    const UINT atlasW   = static_cast<UINT>(data.atlasWidth);
    const UINT atlasH   = static_cast<UINT>(data.atlasHeight);
    const UINT paletteW = TileRegistry::kMaxTileTypes;
    const UINT paletteH = 2;

    if (!CreateTexture2D(mapW, mapH, DXGI_FORMAT_R16_UINT, m_tileIdTexture) ||
        !CreateTexture2D(atlasW, atlasH, DXGI_FORMAT_R8G8B8A8_UNORM, m_atlasTexture) ||
        !CreateTexture2D(paletteW, paletteH, DXGI_FORMAT_R32G32B32A32_FLOAT, m_paletteTexture)) {
        ReleaseTileTextures();
        return false;
    }

    // All three subresources share one staging buffer. Rows are re-packed at
    // 256-byte-aligned pitches and each footprint starts 512-byte aligned.
    struct Upload {
        ID3D12Resource* texture;
        DXGI_FORMAT     format;
        UINT            width, height;
        UINT            rowBytes;  // tightly packed source row
        const uint8_t*  source;
        UINT64          offset;
        UINT            rowPitch;
    };

    Upload uploads[3] = {
        {m_tileIdTexture.Get(), DXGI_FORMAT_R16_UINT, mapW, mapH, mapW * 2u,
         reinterpret_cast<const uint8_t*>(tiles), 0, 0},
        {m_atlasTexture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, atlasW, atlasH, atlasW * 4u,
         data.atlasPixels.data(), 0, 0},
        {m_paletteTexture.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, paletteW, paletteH,
         paletteW * 16u, reinterpret_cast<const uint8_t*>(data.palettePixels.data()), 0, 0},
    };

    UINT64 stagingSize = 0;
    for (Upload& upload : uploads) {
        upload.rowPitch = AlignUp(upload.rowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        stagingSize     = AlignUp64(stagingSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        upload.offset   = stagingSize;
        stagingSize += static_cast<UINT64>(upload.rowPitch) * upload.height;
    }

    ComPtr<ID3D12Resource> staging;
    if (!CreateUploadBuffer(stagingSize, staging)) {
        ReleaseTileTextures();
        return false;
    }

    const D3D12_RANGE noRead{0, 0};
    uint8_t*           mapped = nullptr;
    if (FAILED(staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) {
        LOG_ERROR("[D3D12] Tile staging buffer Map failed");
        ReleaseTileTextures();
        return false;
    }
    for (const Upload& upload : uploads) {
        for (UINT row = 0; row < upload.height; ++row) {
            std::memcpy(mapped + upload.offset + static_cast<UINT64>(row) * upload.rowPitch,
                        upload.source + static_cast<size_t>(row) * upload.rowBytes,
                        upload.rowBytes);
        }
    }
    staging->Unmap(0, nullptr);

    // One-shot upload: the GPU is idle (WaitForGpu above), so the current
    // frame's allocator and the shared command list are free to reuse.
    if (FAILED(m_commandAllocators[m_frameIndex]->Reset()) ||
        FAILED(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr))) {
        LOG_ERROR("[D3D12] Tile upload command list reset failed");
        ReleaseTileTextures();
        return false;
    }

    for (const Upload& upload : uploads) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = upload.texture;
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource                          = staging.Get();
        src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset             = upload.offset;
        src.PlacedFootprint.Footprint.Format   = upload.format;
        src.PlacedFootprint.Footprint.Width    = upload.width;
        src.PlacedFootprint.Footprint.Height   = upload.height;
        src.PlacedFootprint.Footprint.Depth    = 1;
        src.PlacedFootprint.Footprint.RowPitch = upload.rowPitch;

        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER barriers[3] = {};
    for (int i = 0; i < 3; ++i) {
        barriers[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[i].Transition.pResource   = uploads[i].texture;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    m_commandList->ResourceBarrier(3, barriers);

    if (FAILED(m_commandList->Close())) {
        LOG_ERROR("[D3D12] Tile upload command list Close failed");
        ReleaseTileTextures();
        return false;
    }

    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu();  // `staging` dies at end of scope; the copy must be done

    // SRVs into heap slots 0-2; recreation just overwrites them.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip     = 0;
    srv.Texture2D.MipLevels           = 1;
    srv.Texture2D.PlaneSlice          = 0;
    srv.Texture2D.ResourceMinLODClamp = 0.0f;

    srv.Format = DXGI_FORMAT_R16_UINT;
    m_device->CreateShaderResourceView(m_tileIdTexture.Get(), &srv, SrvCpuHandle(0));
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_device->CreateShaderResourceView(m_atlasTexture.Get(), &srv, SrvCpuHandle(1));
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    m_device->CreateShaderResourceView(m_paletteTexture.Get(), &srv, SrvCpuHandle(2));

    // Persistent per-frame upload buffers for region updates, sized for the
    // whole map at aligned pitch and left mapped for their lifetime.
    const UINT   updatePitch = AlignUp(mapW * 2u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 updateSize  = static_cast<UINT64>(updatePitch) * mapH;
    for (UINT i = 0; i < kFrameCount; ++i) {
        if (!CreateUploadBuffer(updateSize, m_tileUploadBuffers[i])) {
            ReleaseTileTextures();
            ReleaseTileUploadBuffers();
            return false;
        }
        if (FAILED(m_tileUploadBuffers[i]->Map(
                0, &noRead, reinterpret_cast<void**>(&m_tileUploadMapped[i])))) {
            LOG_ERROR("[D3D12] Tile update buffer Map failed");
            ReleaseTileTextures();
            ReleaseTileUploadBuffers();
            return false;
        }
    }

    // CPU copy of the grid: UpdateTileRegion patches it, the BeginFrame flush
    // reads from it (the caller's pointer is not retained).
    m_tileCpu.assign(tiles, tiles + static_cast<size_t>(mapWidth) * mapHeight);

    m_mapWidth           = mapWidth;
    m_mapHeight          = mapHeight;
    m_tileResourcesReady = true;

    LOG_INFO("[D3D12] Tile resources created: %dx%d map, %dx%d atlas", mapWidth, mapHeight,
             data.atlasWidth, data.atlasHeight);
    return true;
}

void D3D12Renderer::UpdateTileRegion(int x, int y, int w, int h, const uint16_t* tiles,
                                     int mapWidth)
{
    if (!m_tileResourcesReady || !tiles) {
        return;
    }
    if (mapWidth != m_mapWidth) {
        LOG_WARN("[D3D12] UpdateTileRegion stride %d does not match map width %d; ignored",
                 mapWidth, m_mapWidth);
        return;
    }

    // Clamp to the map, then patch the CPU copy and grow the dirty rect. No
    // GPU work here: the next BeginFrame flushes on the frame's command list.
    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = x + w > m_mapWidth ? m_mapWidth : x + w;
    const int y1 = y + h > m_mapHeight ? m_mapHeight : y + h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int row = y0; row < y1; ++row) {
        std::memcpy(&m_tileCpu[static_cast<size_t>(row) * m_mapWidth + x0],
                    &tiles[static_cast<size_t>(row) * mapWidth + x0],
                    static_cast<size_t>(x1 - x0) * sizeof(uint16_t));
    }

    if (!m_tileUpdatePending) {
        m_dirtyX0 = x0;
        m_dirtyY0 = y0;
        m_dirtyX1 = x1;
        m_dirtyY1 = y1;
        m_tileUpdatePending = true;
    } else {
        if (x0 < m_dirtyX0) m_dirtyX0 = x0;
        if (y0 < m_dirtyY0) m_dirtyY0 = y0;
        if (x1 > m_dirtyX1) m_dirtyX1 = x1;
        if (y1 > m_dirtyY1) m_dirtyY1 = y1;
    }
}

void D3D12Renderer::FlushPendingTileUpload()
{
    if (!m_tileUpdatePending || !m_tileResourcesReady) {
        return;
    }
    m_tileUpdatePending = false;

    uint8_t*        mapped = m_tileUploadMapped[m_frameIndex];
    ID3D12Resource* upload = m_tileUploadBuffers[m_frameIndex].Get();
    if (!mapped || !upload) {
        return;
    }

    // This frame's upload buffer is idle (its fence was waited on in
    // BeginFrame), so the dirty rows can be re-packed at aligned pitch. The
    // rect pitch is at most the full-map pitch the buffer was sized for.
    const UINT w        = static_cast<UINT>(m_dirtyX1 - m_dirtyX0);
    const UINT h        = static_cast<UINT>(m_dirtyY1 - m_dirtyY0);
    const UINT rowBytes = w * static_cast<UINT>(sizeof(uint16_t));
    const UINT rowPitch = AlignUp(rowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

    for (UINT row = 0; row < h; ++row) {
        std::memcpy(mapped + static_cast<UINT64>(row) * rowPitch,
                    &m_tileCpu[(static_cast<size_t>(m_dirtyY0) + row) * m_mapWidth + m_dirtyX0],
                    rowBytes);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_tileIdTexture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource        = m_tileIdTexture.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource                          = upload;
    src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset             = 0;
    src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R16_UINT;
    src.PlacedFootprint.Footprint.Width    = w;
    src.PlacedFootprint.Footprint.Height   = h;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = rowPitch;

    m_commandList->CopyTextureRegion(&dst, static_cast<UINT>(m_dirtyX0),
                                     static_cast<UINT>(m_dirtyY0), 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_commandList->ResourceBarrier(1, &barrier);
}

void D3D12Renderer::BindTilePipeline()
{
    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetGraphicsRootSignature(m_tileRootSignature.Get());
    m_commandList->SetPipelineState(m_tilePipelineState.Get());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->SetGraphicsRootDescriptorTable(1, SrvGpuHandle(0));
    m_boundPipeline = BoundPipeline::Tile;
}

void D3D12Renderer::DrawTileMap(const TileDrawConstants& constants)
{
    if (!m_frameActive || !m_tileResourcesReady || !m_tilePipelineState) {
        return;
    }
    if (m_boundPipeline != BoundPipeline::Tile) {
        BindTilePipeline();
    }

    m_commandList->SetGraphicsRoot32BitConstants(0, kTileConstantCount, &constants, 0);
    m_commandList->DrawInstanced(3, 1, 0, 0);  // fullscreen triangle
}

void D3D12Renderer::ReleaseTileTextures()
{
    m_tileIdTexture.Reset();
    m_atlasTexture.Reset();
    m_paletteTexture.Reset();
}

void D3D12Renderer::ReleaseTileUploadBuffers()
{
    for (UINT i = 0; i < kFrameCount; ++i) {
        if (m_tileUploadBuffers[i] && m_tileUploadMapped[i]) {
            m_tileUploadBuffers[i]->Unmap(0, nullptr);
        }
        m_tileUploadMapped[i] = nullptr;
        m_tileUploadBuffers[i].Reset();
    }
}

void D3D12Renderer::Shutdown()
{
    WaitForGpu();
    m_frameActive = false;

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    ReleaseTileTextures();
    ReleaseTileUploadBuffers();
    m_tilePipelineState.Reset();
    m_tileRootSignature.Reset();
    m_srvHeap.Reset();
    m_tileCpu.clear();
    m_tileResourcesReady = false;
    m_tileUpdatePending  = false;
    m_mapWidth           = 0;
    m_mapHeight          = 0;

    m_fence.Reset();
    m_commandList.Reset();
    for (UINT i = 0; i < kFrameCount; ++i) {
        m_commandAllocators[i].Reset();
    }
    m_pipelineState.Reset();
    m_rootSignature.Reset();
    ReleaseRenderTargets();
    m_rtvHeap.Reset();
    m_swapChain.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_factory.Reset();
}

} // namespace engine
