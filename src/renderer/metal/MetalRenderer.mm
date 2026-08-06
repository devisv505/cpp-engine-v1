#include "renderer/metal/MetalRenderer.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_metal.h>

#include "core/Log.h"
#include "core/Window.h"

// Available under ARC and, unlike an @autoreleasepool block, a pool pushed here
// can span the BeginFrame/EndFrame call pair.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void  objc_autoreleasePoolPop(void* token);

namespace engine {

namespace {

// Quads are generated from the vertex id, so no vertex buffers are involved.
// Metal clip space is Y-up, hence the flip from top-left pixel coordinates.
constexpr const char* kShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct QuadConstants {
    float4 rect;      // x, y, w, h in pixels
    float4 color;
    float2 viewport;  // framebuffer size in pixels
    float2 padding;
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

vertex VertexOut quad_vertex(uint vertexId [[vertex_id]],
                             constant QuadConstants& constants [[buffer(0)]])
{
    float2 corner = float2(float(vertexId & 1u), float((vertexId >> 1) & 1u));
    float2 pixel  = constants.rect.xy + corner * constants.rect.zw;

    VertexOut out;
    out.position = float4(pixel.x / constants.viewport.x * 2.0 - 1.0,
                          1.0 - pixel.y / constants.viewport.y * 2.0,
                          0.0, 1.0);
    out.color = constants.color;
    return out;
}

fragment float4 quad_fragment(VertexOut in [[stage_in]])
{
    return in.color;
}
)";

} // namespace

struct MetalState {
    id<MTLDevice>              device   = nil;
    id<MTLCommandQueue>        queue    = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    CAMetalLayer*              layer    = nil;
    SDL_MetalView              view     = nullptr;

    // Valid only between BeginFrame and EndFrame.
    id<CAMetalDrawable>         drawable      = nil;
    id<MTLCommandBuffer>        commandBuffer = nil;
    id<MTLRenderCommandEncoder> encoder       = nil;
    void*                       poolToken     = nullptr;

    float viewportWidth  = 0.0f;
    float viewportHeight = 0.0f;
};

MetalRenderer::~MetalRenderer()
{
    Shutdown();
}

bool MetalRenderer::Init(Window& window)
{
    m_state = new MetalState();

    m_state->device = MTLCreateSystemDefaultDevice();
    if (!m_state->device) {
        LOG_ERROR("[Metal] MTLCreateSystemDefaultDevice returned nil");
        return false;
    }
    LOG_INFO("[Metal] Device: %s", [[m_state->device name] UTF8String]);

    m_state->queue = [m_state->device newCommandQueue];
    if (!m_state->queue) {
        LOG_ERROR("[Metal] Failed to create command queue");
        return false;
    }
    m_state->queue.label = @"engine.main.queue";

    m_state->view = SDL_Metal_CreateView(window.GetSDLWindow());
    if (!m_state->view) {
        LOG_ERROR("[Metal] SDL_Metal_CreateView failed: %s", SDL_GetError());
        return false;
    }

    // SDL creates the CAMetalLayer but does not associate a device with it.
    m_state->layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(m_state->view);
    if (!m_state->layer) {
        LOG_ERROR("[Metal] SDL_Metal_GetLayer returned null");
        return false;
    }
    m_state->layer.device          = m_state->device;
    m_state->layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    m_state->layer.framebufferOnly = YES;
    m_state->layer.displaySyncEnabled = window.GetConfig().vsync ? YES : NO;

    int pixelWidth  = 0;
    int pixelHeight = 0;
    window.GetPixelSize(pixelWidth, pixelHeight);
    OnResize(pixelWidth, pixelHeight);

    NSError* error = nil;
    id<MTLLibrary> library =
        [m_state->device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource]
                                      options:nil
                                        error:&error];
    if (!library) {
        LOG_ERROR("[Metal] Shader compilation failed: %s",
                  [[error localizedDescription] UTF8String]);
        return false;
    }

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.label                           = @"engine.quad";
    descriptor.vertexFunction                  = [library newFunctionWithName:@"quad_vertex"];
    descriptor.fragmentFunction                = [library newFunctionWithName:@"quad_fragment"];
    descriptor.colorAttachments[0].pixelFormat = m_state->layer.pixelFormat;

    m_state->pipeline = [m_state->device newRenderPipelineStateWithDescriptor:descriptor
                                                                       error:&error];
    if (!m_state->pipeline) {
        LOG_ERROR("[Metal] Pipeline creation failed: %s",
                  [[error localizedDescription] UTF8String]);
        return false;
    }

    LOG_INFO("[Metal] Swapchain and quad pipeline ready (%dx%d, vsync %s)",
             pixelWidth, pixelHeight, window.GetConfig().vsync ? "on" : "off");
    return true;
}

void MetalRenderer::OnResize(int pixelWidth, int pixelHeight)
{
    if (!m_state || !m_state->layer || pixelWidth <= 0 || pixelHeight <= 0) {
        return;
    }
    m_state->viewportWidth  = static_cast<float>(pixelWidth);
    m_state->viewportHeight = static_cast<float>(pixelHeight);
    m_state->layer.drawableSize = CGSizeMake(pixelWidth, pixelHeight);
}

void MetalRenderer::BeginFrame(const Color& clearColor)
{
    if (!m_state || !m_state->pipeline) {
        return;
    }

    m_state->poolToken = objc_autoreleasePoolPush();

    m_state->drawable = [m_state->layer nextDrawable];
    if (!m_state->drawable) {
        // The window is occluded or the swapchain is starved; skip this frame.
        objc_autoreleasePoolPop(m_state->poolToken);
        m_state->poolToken = nullptr;
        return;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture     = m_state->drawable.texture;
    pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(clearColor.r, clearColor.g, clearColor.b, clearColor.a);

    m_state->commandBuffer = [m_state->queue commandBuffer];
    m_state->encoder = [m_state->commandBuffer renderCommandEncoderWithDescriptor:pass];
    [m_state->encoder setRenderPipelineState:m_state->pipeline];
}

void MetalRenderer::DrawQuad(const Quad& quad)
{
    if (!m_state || !m_state->encoder) {
        return;
    }

    const QuadConstants constants{
        {quad.x, quad.y, quad.w, quad.h},
        {quad.color.r, quad.color.g, quad.color.b, quad.color.a},
        {m_state->viewportWidth, m_state->viewportHeight},
        {0.0f, 0.0f},
    };

    [m_state->encoder setVertexBytes:&constants length:sizeof(constants) atIndex:0];
    [m_state->encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                         vertexStart:0
                         vertexCount:4];
}

void MetalRenderer::EndFrame()
{
    if (!m_state || !m_state->poolToken) {
        return;
    }

    [m_state->encoder endEncoding];


    [m_state->commandBuffer presentDrawable:m_state->drawable];
    [m_state->commandBuffer commit];

    m_state->encoder       = nil;
    m_state->commandBuffer = nil;
    m_state->drawable      = nil;

    objc_autoreleasePoolPop(m_state->poolToken);
    m_state->poolToken = nullptr;
}

void MetalRenderer::Shutdown()
{
    if (!m_state) {
        return;
    }
    if (m_state->poolToken) {
        objc_autoreleasePoolPop(m_state->poolToken);
        m_state->poolToken = nullptr;
    }
    if (m_state->view) {
        SDL_Metal_DestroyView(m_state->view);
        m_state->view = nullptr;
    }
    m_state->pipeline = nil;  // ARC releases
    m_state->layer    = nil;
    m_state->queue    = nil;
    m_state->device   = nil;
    delete m_state;
    m_state = nullptr;
}

} // namespace engine
