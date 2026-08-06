#include "renderer/dx12/D3D12Renderer.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <string>

#include <d3d12sdklayers.h>
#include <d3dcompiler.h>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>


#include "core/Log.h"
#include "core/Paths.h"
#include "core/Window.h"
#include "world/Environment.h"
#include "world/TileAtlas.h"
#include "world/TileRegistry.h"

using Microsoft::WRL::ComPtr;

namespace engine {

namespace {

// The tile cbuffer above is written against this exact size; a change to
// TileDrawConstants must update the shader too.
static_assert(sizeof(TileDrawConstants) == 16 * sizeof(uint32_t),
              "TileDrawConstants must stay 16 root constants");

// Root constants for the occluder pass: the wall rect and the mask rect.
struct OccluderPassConstants {
    float rect[4];      // wall x, y, w, h in world pixels
    float maskRect[4];  // mask origin xy, mask size zw
};

// Root constants for the light pass. LightDrawConstants (core/Scene2D.h) only
// covers the light itself; the shader also needs the camera and the mask
// rectangle, so both live in one block. HLSL cbuffer packing puts every field
// at the offset the C++ layout gives it (checked by the static_asserts below).
//
//  DWORD  0- 1  lightPos          DWORD 12-13  camera
//  DWORD  2- 3  lightDir          DWORD 14     zoom
//  DWORD  4- 7  lightColor        DWORD 15     pad0
//  DWORD  8     lightDistance     DWORD 16-17  viewport
//  DWORD  9     cosHalfAngle      DWORD 18-19  pad1
//  DWORD 10     softness          DWORD 20-23  maskRect
//  DWORD 11     mode
struct LightPassConstants {
    float lightPos[2];
    float lightDir[2];
    float lightColor[4];
    float lightDistance;
    float cosHalfAngle;
    float softness;
    float mode;
    float camera[2];
    float zoom;
    float pad0;
    float viewport[2];
    float pad1[2];
    float maskRect[4];
};

constexpr UINT kOccluderConstantCount =
    static_cast<UINT>(sizeof(OccluderPassConstants) / sizeof(uint32_t));
constexpr UINT kLightConstantCount =
    static_cast<UINT>(sizeof(LightPassConstants) / sizeof(uint32_t));

// Root constants are limited to 64 DWORDs, minus one per other root parameter.
static_assert(kOccluderConstantCount == 8, "occluder constants must stay 8 DWORDs");
static_assert(kLightConstantCount == 24, "light constants must stay 24 DWORDs");
static_assert(offsetof(LightPassConstants, camera) == 12 * sizeof(float),
              "light cbuffer packing: camera must land on DWORD 12");
static_assert(offsetof(LightPassConstants, maskRect) == 20 * sizeof(float),
              "light cbuffer packing: maskRect must land on DWORD 20");

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

// Circle (light position and reach) against the world-space viewport rect.
bool CircleIntersectsRect(float cx, float cy, float radius, float minX, float minY, float maxX,
                          float maxY)
{
    const float nearestX = cx < minX ? minX : (cx > maxX ? maxX : cx);
    const float nearestY = cy < minY ? minY : (cy > maxY ? maxY : cy);
    const float dx       = cx - nearestX;
    const float dy       = cy - nearestY;
    return dx * dx + dy * dy <= radius * radius;
}

// Everything the occluder and light pipelines share: no depth, no input
// layout, solid fill, no culling, one render target, no MSAA, opaque blending.
// Callers still set pRootSignature, VS, PS, RTVFormats[0] and any blending.
void FillFixedFunctionState(D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    desc.BlendState.AlphaToCoverageEnable  = FALSE;
    desc.BlendState.IndependentBlendEnable = FALSE;

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
    desc.DSVFormat                      = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count               = 1;
    desc.SampleDesc.Quality             = 0;
    desc.NodeMask                       = 0;
    desc.Flags                          = D3D12_PIPELINE_STATE_FLAG_NONE;
}

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

// Loads the HLSL from shaders/hlsl/<name>.hlsl and compiles it at runtime, so
// editing a shader only needs an app relaunch, not a rebuild.
bool CompileShader(const char* sourceName, const char* entryPoint, const char* target,
                   ComPtr<ID3DBlob>& blob)
{
    const std::string path   = ResolveDataPath(std::string("shaders/hlsl/") + sourceName);
    const std::string source = LoadTextFile(path);
    if (source.empty()) {
        LOG_ERROR("[D3D12] Shader source '%s' is missing or empty", path.c_str());
        return false;
    }
    const size_t sourceSize = source.size();

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source.data(), sourceSize, sourceName, nullptr, nullptr, entryPoint,
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
        // kFrameCount back-buffer views plus one slot (kMaskRtvIndex) for the
        // occlusion mask. Only the back-buffer views are rewritten on resize.
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = kRtvHeapSize;
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
    if (!CompileShader("quad.hlsl", "VSMain",
                       "vs_5_1", vertexShader)) {
        return false;
    }
    if (!CompileShader("quad.hlsl", "PSMain",
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

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::RtvCpuHandle(UINT slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * static_cast<SIZE_T>(m_rtvDescriptorSize);
    return handle;
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

bool D3D12Renderer::CreateRenderTargetTexture(UINT width, UINT height, DXGI_FORMAT format,
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
    desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // Matches the ClearRenderTargetView the occluder pass issues, so the clear
    // stays on the fast path and the debug layer stays quiet.
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format   = format;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 0.0f;

    // Created straight in RENDER_TARGET state: the only thing that ever writes
    // it is the occluder pass, which runs before the first read.
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
                                                 IID_PPV_ARGS(&texture)))) {
        LOG_ERROR("[D3D12] CreateCommittedResource(render target %ux%u) failed", width, height);
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
    if (!CompileShader("tile.hlsl", "VSMain",
                       "vs_5_1", vertexShader)) {
        return false;
    }
    if (!CompileShader("tile.hlsl", "PSMain",
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
    if (!m_frameActive) {
        return;
    }
    // DrawWalls receives no camera of its own; it transforms with this one.
    m_lastCamera  = constants;
    m_cameraValid = true;

    if (!m_tileResourcesReady || !m_tilePipelineState) {
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

// --- Volumetric lighting -----------------------------------------------------

bool D3D12Renderer::CreateOccluderRootSignature()
{
    // One block of root constants at b0 (wall rect + mask rect); the pass has
    // no textures and no vertex buffers.
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.RegisterSpace  = 0;
    param.Constants.Num32BitValues = kOccluderConstantCount;
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
        LOG_ERROR("[D3D12] Occluder root signature serialization failed: %s",
                  errors ? static_cast<const char*>(errors->GetBufferPointer())
                         : "(no diagnostics)");
        return false;
    }

    if (FAILED(m_device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                             serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&m_occluderRootSignature)))) {
        LOG_ERROR("[D3D12] Occluder CreateRootSignature failed");
        return false;
    }
    return true;
}

bool D3D12Renderer::CreateOccluderPipelineState()
{
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    if (!CompileShader("occluder.hlsl",
                       "VSMain", "vs_5_1", vertexShader)) {
        return false;
    }
    if (!CompileShader("occluder.hlsl",
                       "PSMain", "ps_5_1", pixelShader)) {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    FillFixedFunctionState(desc);
    desc.pRootSignature = m_occluderRootSignature.Get();

    desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
    desc.VS.BytecodeLength  = vertexShader->GetBufferSize();
    desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
    desc.PS.BytecodeLength  = pixelShader->GetBufferSize();

    // Overlapping walls all write 1.0, so the mask needs no blending.
    desc.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;

    if (FAILED(
            m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_occluderPipelineState)))) {
        LOG_ERROR("[D3D12] Occluder CreateGraphicsPipelineState failed");
        return false;
    }
    return true;
}

bool D3D12Renderer::CreateLightRootSignature()
{
    // Param 0: LightPassConstants as root constants at b0 (24 DWORDs; the
    // pass reuses the block and fills only its first 16). Param 1: the
    // occlusion mask at t0, plus a linear-clamp static sampler at s0.
    // 24 + 1 = 25 of the 64 available root DWORDs.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors                    = 1;
    range.BaseShaderRegister                = 0;
    range.RegisterSpace                     = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace  = 0;
    params[0].Constants.Num32BitValues = kLightConstantCount;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &range;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
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
        LOG_ERROR("[D3D12] Light root signature serialization failed: %s",
                  errors ? static_cast<const char*>(errors->GetBufferPointer())
                         : "(no diagnostics)");
        return false;
    }

    if (FAILED(m_device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                             serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&m_lightRootSignature)))) {
        LOG_ERROR("[D3D12] Light CreateRootSignature failed");
        return false;
    }
    return true;
}

bool D3D12Renderer::CreateLightPipelineState()
{
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    if (!CompileShader("light.hlsl", "VSMain",
                       "vs_5_1", vertexShader)) {
        return false;
    }
    if (!CompileShader("light.hlsl", "PSMain",
                       "ps_5_1", pixelShader)) {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    FillFixedFunctionState(desc);
    desc.pRootSignature = m_lightRootSignature.Get();

    desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
    desc.VS.BytecodeLength  = vertexShader->GetBufferSize();
    desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
    desc.PS.BytecodeLength  = pixelShader->GetBufferSize();

    // Lights accumulate: every light adds its contribution to what is there.
    D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
    blend.BlendEnable    = TRUE;
    blend.SrcBlend       = D3D12_BLEND_ONE;
    blend.DestBlend      = D3D12_BLEND_ONE;
    blend.BlendOp        = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha  = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha   = D3D12_BLEND_OP_ADD;

    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

    if (FAILED(m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_lightPipelineState)))) {
        LOG_ERROR("[D3D12] Light CreateGraphicsPipelineState failed");
        return false;
    }
    return true;
}

bool D3D12Renderer::EnsureOccluderPipeline()
{
    if (!m_occluderRootSignature && !CreateOccluderRootSignature()) {
        return false;
    }
    if (!m_occluderPipelineState && !CreateOccluderPipelineState()) {
        return false;
    }
    return true;
}

bool D3D12Renderer::EnsureLightingPipelines()
{
    if (m_lightPipelineState) {
        return true;
    }
    // A shader that failed to compile will fail again; do not pay for it once
    // per frame.
    if (m_lightingUnavailable || !m_device) {
        return false;
    }

    if ((!m_lightRootSignature && !CreateLightRootSignature()) ||
        (!m_lightPipelineState && !CreateLightPipelineState()) ||) {
        LOG_ERROR("[D3D12] Lighting pipeline unavailable; light passes are disabled");
        m_lightingUnavailable = true;
        m_lightPipelineState.Reset();
        m_lightRootSignature.Reset();
        return false;
    }
    return true;
}

void D3D12Renderer::ReleaseOcclusionMask()
{
    // The RTV and SRV descriptors are left dangling on purpose: nothing binds
    // them while m_maskReady is false, and the next successful SetOccluders
    // overwrites both in place.
    m_maskReady = false;
    m_occlusionMask.Reset();
    m_maskWidth  = 0;
    m_maskHeight = 0;
}

void D3D12Renderer::SetOccluders(const Wall* walls, int wallCount, float originX, float originY,
                                 float worldWidth, float worldHeight)
{
    if (!m_device || !m_commandQueue || !m_commandList) {
        LOG_ERROR("[D3D12] SetOccluders called before Init");
        return;
    }
    if (m_frameActive) {
        LOG_ERROR("[D3D12] SetOccluders must not run inside a frame");
        return;
    }
    if (!(worldWidth > 0.0f) || !(worldHeight > 0.0f)) {
        LOG_ERROR("[D3D12] SetOccluders: invalid world rectangle (%.1f x %.1f)", worldWidth,
                  worldHeight);
        return;
    }

    // In-flight frames may still be sampling the previous mask, and the shared
    // command list has to be idle before this can reuse it.
    WaitForGpu();
    ReleaseOcclusionMask();

    if (!CreateSrvHeap() || !EnsureOccluderPipeline()) {
        return;
    }
    // The light pipeline is built here too: this runs outside a
    // frame, so their shader compiles never stall a recorded command list.
    EnsureLightingPipelines();

    // One texel per four world pixels, at least 1 and at most kMaskMaxTexels.
    // The comparisons are written so a NaN extent lands on the 1-texel floor.
    const auto resolution = [](float worldExtent) -> UINT {
        float texels = std::ceil(worldExtent / kMaskWorldPxPerTexel);
        if (!(texels >= 1.0f)) {
            texels = 1.0f;
        }
        if (texels > static_cast<float>(kMaskMaxTexels)) {
            texels = static_cast<float>(kMaskMaxTexels);
        }
        return static_cast<UINT>(texels);
    };

    const UINT maskW = resolution(worldWidth);
    const UINT maskH = resolution(worldHeight);

    if (!CreateRenderTargetTexture(maskW, maskH, DXGI_FORMAT_R8_UNORM, m_occlusionMask)) {
        ReleaseOcclusionMask();
        return;
    }

    // The mask RTV sits in the slot past the back buffers, so a resize (which
    // only rewrites slots 0..kFrameCount-1) leaves it alone.
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format               = DXGI_FORMAT_R8_UNORM;
    rtvDesc.ViewDimension        = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice   = 0;
    rtvDesc.Texture2D.PlaneSlice = 0;

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvCpuHandle(kMaskRtvIndex);
    m_device->CreateRenderTargetView(m_occlusionMask.Get(), &rtvDesc, rtv);

    // SRV heap slot 3, right after the three tile textures.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                        = DXGI_FORMAT_R8_UNORM;
    srv.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip     = 0;
    srv.Texture2D.MipLevels           = 1;
    srv.Texture2D.PlaneSlice          = 0;
    srv.Texture2D.ResourceMinLODClamp = 0.0f;
    m_device->CreateShaderResourceView(m_occlusionMask.Get(), &srv, SrvCpuHandle(kMaskSrvSlot));

    // One-shot command list: the GPU is idle after the WaitForGpu above, so the
    // current frame's allocator and the shared list are free to reuse.
    if (FAILED(m_commandAllocators[m_frameIndex]->Reset()) ||
        FAILED(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr))) {
        LOG_ERROR("[D3D12] Occluder command list reset failed");
        ReleaseOcclusionMask();
        return;
    }

    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width    = static_cast<float>(maskW);
    viewport.Height   = static_cast<float>(maskH);
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;

    D3D12_RECT scissor{};
    scissor.left   = 0;
    scissor.top    = 0;
    scissor.right  = static_cast<LONG>(maskW);
    scissor.bottom = static_cast<LONG>(maskH);

    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float unoccluded[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_commandList->ClearRenderTargetView(rtv, unoccluded, 0, nullptr);

    int occluders = 0;
    if (walls && wallCount > 0) {
        m_commandList->SetGraphicsRootSignature(m_occluderRootSignature.Get());
        m_commandList->SetPipelineState(m_occluderPipelineState.Get());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        OccluderPassConstants constants{};
        constants.maskRect[0] = originX;
        constants.maskRect[1] = originY;
        constants.maskRect[2] = worldWidth;
        constants.maskRect[3] = worldHeight;

        for (int i = 0; i < wallCount; ++i) {
            const Wall& wall = walls[i];
            if (!wall.blocksLight || wall.w <= 0.0f || wall.h <= 0.0f) {
                continue;
            }
            constants.rect[0] = wall.x;
            constants.rect[1] = wall.y;
            constants.rect[2] = wall.w;
            constants.rect[3] = wall.h;

            m_commandList->SetGraphicsRoot32BitConstants(0, kOccluderConstantCount, &constants, 0);
            m_commandList->DrawInstanced(4, 1, 0, 0);  // triangle strip over the four corners
            ++occluders;
        }
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_occlusionMask.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_commandList->ResourceBarrier(1, &barrier);

    if (FAILED(m_commandList->Close())) {
        LOG_ERROR("[D3D12] Occluder command list Close failed");
        ReleaseOcclusionMask();
        return;
    }

    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu();  // the mask must be finished before any frame samples it

    m_maskOriginX = originX;
    m_maskOriginY = originY;
    m_maskWorldW  = worldWidth;
    m_maskWorldH  = worldHeight;
    m_maskWidth   = maskW;
    m_maskHeight  = maskH;
    m_maskReady   = true;

    // The next BeginFrame resets the list anyway; keep the tracker honest.
    m_boundPipeline = BoundPipeline::None;

    LOG_INFO("[D3D12] Occlusion mask rebuilt: %ux%u texels over %.0fx%.0f world px, %d occluders",
             maskW, maskH, worldWidth, worldHeight, occluders);
}

void D3D12Renderer::DrawWalls(const Wall* walls, int wallCount)
{
    if (!m_frameActive || !walls || wallCount <= 0 || !m_cameraValid) {
        return;
    }

    // World -> screen happens here on the CPU, so the walls go straight through
    // the existing quad pipeline and need no shader of their own.
    const float zoom  = m_lastCamera.zoom > 1e-6f ? m_lastCamera.zoom : 1.0f;
    const float halfW = static_cast<float>(m_width) * 0.5f;
    const float halfH = static_cast<float>(m_height) * 0.5f;

    for (int i = 0; i < wallCount; ++i) {
        const Wall& wall = walls[i];
        if (wall.w <= 0.0f || wall.h <= 0.0f) {
            continue;
        }

        Quad quad;
        quad.x     = (wall.x - m_lastCamera.cameraX) * zoom + halfW;
        quad.y     = (wall.y - m_lastCamera.cameraY) * zoom + halfH;
        quad.w     = wall.w * zoom;
        quad.h     = wall.h * zoom;
        quad.color = wall.color;

        // Off-screen walls would rasterize to nothing; skip the root-constant
        // upload and the draw call instead.
        if (quad.x + quad.w < 0.0f || quad.y + quad.h < 0.0f ||
            quad.x > static_cast<float>(m_width) || quad.y > static_cast<float>(m_height)) {
            continue;
        }
        DrawQuad(quad);
    }
}

void D3D12Renderer::BindLightPipeline()
{
    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetGraphicsRootSignature(m_lightRootSignature.Get());
    m_commandList->SetPipelineState(m_lightPipelineState.Get());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->SetGraphicsRootDescriptorTable(1, SrvGpuHandle(kMaskSrvSlot));
    m_boundPipeline = BoundPipeline::Light;
}

void D3D12Renderer::DrawLighting(const Light* lights, int lightCount,
                                 const TileDrawConstants& camera)
{
    if (!m_frameActive || !EnsureLightingPipelines()) {
        return;
    }

    // The camera the tile pass used, so the lighting lines up with the map.
    const float zoom  = camera.zoom > 1e-6f ? camera.zoom : 1.0f;
    const float viewW = camera.viewportW > 0.0f ? camera.viewportW : static_cast<float>(m_width);
    const float viewH = camera.viewportH > 0.0f ? camera.viewportH : static_cast<float>(m_height);

    if (lights && lightCount > 0 && m_maskReady) {
        // World-space rectangle the viewport covers; lights whose reach misses
        // it never touch the screen.
        const float halfWorldW = viewW * 0.5f / zoom;
        const float halfWorldH = viewH * 0.5f / zoom;
        const float minX       = camera.cameraX - halfWorldW;
        const float maxX       = camera.cameraX + halfWorldW;
        const float minY       = camera.cameraY - halfWorldH;
        const float maxY       = camera.cameraY + halfWorldH;

        LightPassConstants constants{};
        constants.camera[0]   = camera.cameraX;
        constants.camera[1]   = camera.cameraY;
        constants.zoom        = zoom;
        constants.viewport[0] = viewW;
        constants.viewport[1] = viewH;
        constants.maskRect[0] = m_maskOriginX;
        constants.maskRect[1] = m_maskOriginY;
        constants.maskRect[2] = m_maskWorldW;
        constants.maskRect[3] = m_maskWorldH;

        // Cones first, then the screen-space god rays; both accumulate through
        // the same additive pipeline, so the split costs nothing.
        for (int pass = 0; pass < 2; ++pass) {
            const LightMode wanted =
                pass == 0 ? LightMode::VolumetricCone : LightMode::ScreenSpace;

            for (int i = 0; i < lightCount; ++i) {
                const Light& light = lights[i];
                if (light.mode != wanted || light.distance <= 0.0f || light.intensity <= 0.0f) {
                    continue;
                }
                if (!CircleIntersectsRect(light.x, light.y, light.distance, minX, minY, maxX,
                                          maxY)) {
                    continue;
                }

                float       dirX   = light.dirX;
                float       dirY   = light.dirY;
                const float dirLen = std::sqrt(dirX * dirX + dirY * dirY);
                if (dirLen > 1e-6f) {
                    dirX /= dirLen;
                    dirY /= dirLen;
                } else {
                    dirX = 1.0f;
                    dirY = 0.0f;
                }

                float angleDeg = light.angleDeg;
                if (angleDeg < 0.0f)   angleDeg = 0.0f;
                if (angleDeg > 360.0f) angleDeg = 360.0f;

                float softness = light.softness;
                if (softness < 0.0f) softness = 0.0f;
                if (softness > 1.0f) softness = 1.0f;

                constants.lightPos[0]   = light.x;
                constants.lightPos[1]   = light.y;
                constants.lightDir[0]   = dirX;
                constants.lightDir[1]   = dirY;
                constants.lightColor[0] = light.color.r * light.intensity;
                constants.lightColor[1] = light.color.g * light.intensity;
                constants.lightColor[2] = light.color.b * light.intensity;
                constants.lightColor[3] = light.color.a;
                constants.lightDistance = light.distance;
                constants.cosHalfAngle  = std::cos(angleDeg * 0.5f * kDegToRad);
                constants.softness      = softness;
                constants.mode          = wanted == LightMode::ScreenSpace ? 1.0f : 0.0f;

                if (m_boundPipeline != BoundPipeline::Light) {
                    BindLightPipeline();
                }
                m_commandList->SetGraphicsRoot32BitConstants(0, kLightConstantCount, &constants, 0);
                m_commandList->DrawInstanced(3, 1, 0, 0);  // fullscreen triangle
            }
        }
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

    // Lighting first: the mask holds an RTV in the shared RTV heap and an SRV
    // in the shared SRV heap, so it goes before either heap is dropped.
    ReleaseOcclusionMask();
    m_lightPipelineState.Reset();
    m_lightRootSignature.Reset();
    m_occluderPipelineState.Reset();
    m_occluderRootSignature.Reset();
    m_lightingUnavailable = false;
    m_cameraValid         = false;
    m_lastCamera          = TileDrawConstants{};

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
