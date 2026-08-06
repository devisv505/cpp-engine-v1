#include "renderer/dx12/D3D12Renderer.h"

#include <climits>
#include <string>

#include <d3d12sdklayers.h>
#include <d3dcompiler.h>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include "core/Log.h"
#include "core/Window.h"

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

bool CompileShader(const char* entryPoint, const char* target, ComPtr<ID3DBlob>& blob)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kQuadShaderSource, sizeof(kQuadShaderSource) - 1, "quad.hlsl",
                                  nullptr, nullptr, entryPoint, target, flags, 0, &blob, &errors);
    if (FAILED(hr)) {
        LOG_ERROR("[D3D12] %s compilation failed (0x%08X): %s", target, static_cast<unsigned>(hr),
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
    if (!CompileShader("VSMain", "vs_5_1", vertexShader)) {
        return false;
    }
    if (!CompileShader("PSMain", "ps_5_1", pixelShader)) {
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
    if (FAILED(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(),
                                    m_pipelineState.Get()))) {
        LOG_ERROR("[D3D12] Command list reset failed");
        return;
    }

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

    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetPipelineState(m_pipelineState.Get());
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissor);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    m_frameActive = true;
}

void D3D12Renderer::DrawQuad(const Quad& quad)
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

void D3D12Renderer::Shutdown()
{
    WaitForGpu();
    m_frameActive = false;

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

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
