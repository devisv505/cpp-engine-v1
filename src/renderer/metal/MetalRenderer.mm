#include "renderer/metal/MetalRenderer.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_metal.h>

#include <imgui.h>
#include <imgui_impl_metal.h>
#include <imgui_impl_sdl3.h>

#include "core/Log.h"
#include "core/Window.h"
#include "world/TileAtlas.h"

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

// One fullscreen pass renders the entire visible map: each fragment finds its
// world tile from the camera, fetches the id, and resolves the tile's color
// or atlas texture through the palette. Layout mirrors TileDrawConstants.
constexpr const char* kTileShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct TileConstants {
    float2 camera;
    float  zoom;
    float  tileSizePx;
    float2 viewport;
    float2 mapSize;
    float4 background;
    float4 padding;
};

vertex float4 tile_vertex(uint vertexId [[vertex_id]])
{
    // Fullscreen triangle: (-1,-1) (3,-1) (-1,3).
    float2 corner = float2(float((vertexId << 1) & 2u), float(vertexId & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

fragment float4 tile_fragment(float4 position [[position]],
                              constant TileConstants& c [[buffer(0)]],
                              texture2d<uint>  tileIds [[texture(0)]],
                              texture2d<float> atlas   [[texture(1)]],
                              texture2d<float> palette [[texture(2)]])
{
    float2 worldPx  = c.camera + (position.xy - c.viewport * 0.5) / c.zoom;
    float2 tilePos  = floor(worldPx / c.tileSizePx);
    if (tilePos.x < 0.0 || tilePos.y < 0.0 ||
        tilePos.x >= c.mapSize.x || tilePos.y >= c.mapSize.y) {
        return c.background;
    }

    uint id = min(tileIds.read(uint2(tilePos)).r, 255u);
    float4 colorRow = palette.read(uint2(id, 0));
    float4 uvRow    = palette.read(uint2(id, 1));

    float3 result = colorRow.rgb;
    if (colorRow.a > 0.5) {
        constexpr sampler pointClamp(filter::nearest, address::clamp_to_edge);
        float2 f  = fract(worldPx / c.tileSizePx);
        float2 uv = mix(uvRow.xy, uvRow.zw, f);
        result = atlas.sample(pointClamp, uv).rgb * colorRow.rgb;
    }
    return float4(result, 1.0);
}
)";

} // namespace

struct MetalState {
    id<MTLDevice>              device   = nil;
    id<MTLCommandQueue>        queue    = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    CAMetalLayer*              layer    = nil;
    SDL_MetalView              view     = nullptr;

    // Tile-map pass. The pipeline is created at Init; the textures are
    // (re)created per map by CreateTileResources.
    id<MTLRenderPipelineState> tilePipeline = nil;
    id<MTLTexture>             tileIdTex    = nil;
    id<MTLTexture>             atlasTex     = nil;
    id<MTLTexture>             paletteTex   = nil;

    bool imguiReady = false;

    // Valid only between BeginFrame and EndFrame.
    id<CAMetalDrawable>         drawable       = nil;
    id<MTLCommandBuffer>        commandBuffer  = nil;
    id<MTLRenderCommandEncoder> encoder        = nil;
    MTLRenderPassDescriptor*    passDescriptor = nil;
    void*                       poolToken      = nullptr;

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

    id<MTLLibrary> tileLibrary =
        [m_state->device newLibraryWithSource:[NSString stringWithUTF8String:kTileShaderSource]
                                      options:nil
                                        error:&error];
    if (!tileLibrary) {
        LOG_ERROR("[Metal] Tile shader compilation failed: %s",
                  [[error localizedDescription] UTF8String]);
        return false;
    }

    MTLRenderPipelineDescriptor* tileDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    tileDescriptor.label                           = @"engine.tilemap";
    tileDescriptor.vertexFunction                  = [tileLibrary newFunctionWithName:@"tile_vertex"];
    tileDescriptor.fragmentFunction                = [tileLibrary newFunctionWithName:@"tile_fragment"];
    tileDescriptor.colorAttachments[0].pixelFormat = m_state->layer.pixelFormat;

    m_state->tilePipeline = [m_state->device newRenderPipelineStateWithDescriptor:tileDescriptor
                                                                            error:&error];
    if (!m_state->tilePipeline) {
        LOG_ERROR("[Metal] Tile pipeline creation failed: %s",
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

    m_state->commandBuffer  = [m_state->queue commandBuffer];
    m_state->encoder        = [m_state->commandBuffer renderCommandEncoderWithDescriptor:pass];
    m_state->passDescriptor = pass;  // ImGui's Metal backend needs it per frame
    [m_state->encoder setRenderPipelineState:m_state->pipeline];
}

bool MetalRenderer::CreateTileResources(const TileRenderData& data,
                                        int mapWidth, int mapHeight,
                                        const uint16_t* tiles)
{
    if (!m_state || !m_state->device) {
        return false;
    }

    // Shared storage keeps updates a plain CPU-side replaceRegion; command
    // buffers retain the old textures, so replacing them mid-flight is safe.
    MTLTextureDescriptor* idDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Uint
                                                           width:mapWidth
                                                          height:mapHeight
                                                       mipmapped:NO];
    idDesc.storageMode = MTLStorageModeShared;
    m_state->tileIdTex = [m_state->device newTextureWithDescriptor:idDesc];

    MTLTextureDescriptor* atlasDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:data.atlasWidth
                                                          height:data.atlasHeight
                                                       mipmapped:NO];
    atlasDesc.storageMode = MTLStorageModeShared;
    m_state->atlasTex = [m_state->device newTextureWithDescriptor:atlasDesc];

    MTLTextureDescriptor* paletteDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                           width:TileRegistry::kMaxTileTypes
                                                          height:2
                                                       mipmapped:NO];
    paletteDesc.storageMode = MTLStorageModeShared;
    m_state->paletteTex = [m_state->device newTextureWithDescriptor:paletteDesc];

    if (!m_state->tileIdTex || !m_state->atlasTex || !m_state->paletteTex) {
        LOG_ERROR("[Metal] Tile texture creation failed");
        return false;
    }

    [m_state->tileIdTex replaceRegion:MTLRegionMake2D(0, 0, mapWidth, mapHeight)
                          mipmapLevel:0
                            withBytes:tiles
                          bytesPerRow:static_cast<NSUInteger>(mapWidth) * sizeof(uint16_t)];
    [m_state->atlasTex replaceRegion:MTLRegionMake2D(0, 0, data.atlasWidth, data.atlasHeight)
                         mipmapLevel:0
                           withBytes:data.atlasPixels.data()
                         bytesPerRow:static_cast<NSUInteger>(data.atlasWidth) * 4];
    [m_state->paletteTex replaceRegion:MTLRegionMake2D(0, 0, TileRegistry::kMaxTileTypes, 2)
                           mipmapLevel:0
                             withBytes:data.palettePixels.data()
                           bytesPerRow:TileRegistry::kMaxTileTypes * 4 * sizeof(float)];

    LOG_INFO("[Metal] Tile resources ready: %dx%d map, %dx%d atlas",
             mapWidth, mapHeight, data.atlasWidth, data.atlasHeight);
    return true;
}

void MetalRenderer::UpdateTileRegion(int x, int y, int w, int h,
                                     const uint16_t* tiles, int mapWidth)
{
    if (!m_state || !m_state->tileIdTex || w <= 0 || h <= 0) {
        return;
    }
    // Row-by-row so the source pointer walks the full map array.
    for (int row = 0; row < h; ++row) {
        [m_state->tileIdTex replaceRegion:MTLRegionMake2D(x, y + row, w, 1)
                              mipmapLevel:0
                                withBytes:tiles + static_cast<size_t>(y + row) * mapWidth + x
                              bytesPerRow:static_cast<NSUInteger>(w) * sizeof(uint16_t)];
    }
}

void MetalRenderer::DrawTileMap(const TileDrawConstants& constants)
{
    if (!m_state || !m_state->encoder || !m_state->tileIdTex) {
        return;
    }
    [m_state->encoder setRenderPipelineState:m_state->tilePipeline];
    [m_state->encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
    [m_state->encoder setFragmentTexture:m_state->tileIdTex atIndex:0];
    [m_state->encoder setFragmentTexture:m_state->atlasTex atIndex:1];
    [m_state->encoder setFragmentTexture:m_state->paletteTex atIndex:2];
    [m_state->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    // Back to the quad pipeline: DrawQuad assumes it is current.
    [m_state->encoder setRenderPipelineState:m_state->pipeline];
}

bool MetalRenderer::InitImGui(Window& window)
{
    if (!m_state || !m_state->device) {
        return false;
    }
    if (!ImGui_ImplSDL3_InitForMetal(window.GetSDLWindow())) {
        LOG_ERROR("[Metal] ImGui SDL3 init failed");
        return false;
    }
    if (!ImGui_ImplMetal_Init(m_state->device)) {
        LOG_ERROR("[Metal] ImGui Metal init failed");
        ImGui_ImplSDL3_Shutdown();
        return false;
    }
    m_state->imguiReady = true;
    return true;
}

void MetalRenderer::BeginImGuiFrame()
{
    if (m_state && m_state->imguiReady && m_state->passDescriptor) {
        ImGui_ImplMetal_NewFrame(m_state->passDescriptor);
    }
}

void MetalRenderer::RenderImGui()
{
    if (m_state && m_state->imguiReady && m_state->encoder) {
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(),
                                       m_state->commandBuffer, m_state->encoder);
    }
}

void MetalRenderer::ShutdownImGui()
{
    if (m_state && m_state->imguiReady) {
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        m_state->imguiReady = false;
    }
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

    m_state->encoder        = nil;
    m_state->commandBuffer  = nil;
    m_state->drawable       = nil;
    m_state->passDescriptor = nil;

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
    ShutdownImGui();
    if (m_state->view) {
        SDL_Metal_DestroyView(m_state->view);
        m_state->view = nullptr;
    }
    m_state->tilePipeline = nil;  // ARC releases
    m_state->tileIdTex    = nil;
    m_state->atlasTex     = nil;
    m_state->paletteTex   = nil;
    m_state->pipeline     = nil;
    m_state->layer        = nil;
    m_state->queue        = nil;
    m_state->device       = nil;
    delete m_state;
    m_state = nullptr;
}

} // namespace engine
