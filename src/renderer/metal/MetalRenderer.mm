#include "renderer/metal/MetalRenderer.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_metal.h>

#include <algorithm>
#include <cmath>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Window.h"
#include "world/Environment.h"
#include "world/TileAtlas.h"

// Available under ARC and, unlike an @autoreleasepool block, a pool pushed here
// can span the BeginFrame/EndFrame call pair.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void  objc_autoreleasePoolPop(void* token);

namespace engine {

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

    // Volumetric lighting. The occlusion mask is cached in world space and
    // rebuilt only when the wall set changes.
    id<MTLRenderPipelineState> occluderPipeline = nil;
    id<MTLRenderPipelineState> lightPipeline    = nil;
    id<MTLTexture>             occlusionMask    = nil;
    float maskOriginX = 0.0f, maskOriginY = 0.0f;
    float maskWidth   = 1.0f, maskHeight   = 1.0f;

    // Camera from the most recent DrawTileMap, reused to place world-space
    // quads (walls) without threading it through every call.
    float cameraX = 0.0f, cameraY = 0.0f, cameraZoom = 1.0f;

    // Valid only between BeginFrame and EndFrame.
    id<CAMetalDrawable>         drawable       = nil;
    id<MTLCommandBuffer>        commandBuffer  = nil;
    id<MTLRenderCommandEncoder> encoder        = nil;
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
        [m_state->device newLibraryWithSource:[NSString stringWithUTF8String:LoadTextFile(ResolveDataPath("shaders/metal/quad.metal")).c_str()]
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
        [m_state->device newLibraryWithSource:[NSString stringWithUTF8String:LoadTextFile(ResolveDataPath("shaders/metal/tile.metal")).c_str()]
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

    if (!CreateLightingPipelines()) {
        return false;
    }

    LOG_INFO("[Metal] Swapchain, tile and lighting pipelines ready (%dx%d, vsync %s)",
             pixelWidth, pixelHeight, window.GetConfig().vsync ? "on" : "off");
    return true;
}

bool MetalRenderer::CreateLightingPipelines()
{
    NSError* error = nil;

    id<MTLLibrary> occluderLibrary =
        [m_state->device newLibraryWithSource:[NSString stringWithUTF8String:LoadTextFile(ResolveDataPath("shaders/metal/occluder.metal")).c_str()]
                                      options:nil
                                        error:&error];
    if (!occluderLibrary) {
        LOG_ERROR("[Metal] Occluder shader compilation failed: %s",
                  [[error localizedDescription] UTF8String]);
        return false;
    }

    MTLRenderPipelineDescriptor* occluder = [[MTLRenderPipelineDescriptor alloc] init];
    occluder.label                           = @"engine.occluder";
    occluder.vertexFunction                  = [occluderLibrary newFunctionWithName:@"occluder_vertex"];
    occluder.fragmentFunction                = [occluderLibrary newFunctionWithName:@"occluder_fragment"];
    occluder.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
    m_state->occluderPipeline =
        [m_state->device newRenderPipelineStateWithDescriptor:occluder error:&error];
    if (!m_state->occluderPipeline) {
        LOG_ERROR("[Metal] Occluder pipeline failed: %s",
                  [[error localizedDescription] UTF8String]);
        return false;
    }

    id<MTLLibrary> lightLibrary =
        [m_state->device newLibraryWithSource:[NSString stringWithUTF8String:LoadTextFile(ResolveDataPath("shaders/metal/lighting.metal")).c_str()]
                                      options:nil
                                        error:&error];
    if (!lightLibrary) {
        LOG_ERROR("[Metal] Light shader compilation failed: %s",
                  [[error localizedDescription] UTF8String]);
        return false;
    }

    // Lights accumulate additively into the scene.
    MTLRenderPipelineDescriptor* light = [[MTLRenderPipelineDescriptor alloc] init];
    light.label                                            = @"engine.light";
    light.vertexFunction                                   = [lightLibrary newFunctionWithName:@"fullscreen_vertex"];
    light.fragmentFunction                                 = [lightLibrary newFunctionWithName:@"light_fragment"];
    light.colorAttachments[0].pixelFormat                  = m_state->layer.pixelFormat;
    light.colorAttachments[0].blendingEnabled              = YES;
    light.colorAttachments[0].rgbBlendOperation            = MTLBlendOperationAdd;
    light.colorAttachments[0].alphaBlendOperation          = MTLBlendOperationAdd;
    light.colorAttachments[0].sourceRGBBlendFactor         = MTLBlendFactorOne;
    light.colorAttachments[0].destinationRGBBlendFactor    = MTLBlendFactorOne;
    light.colorAttachments[0].sourceAlphaBlendFactor       = MTLBlendFactorZero;
    light.colorAttachments[0].destinationAlphaBlendFactor  = MTLBlendFactorOne;
    m_state->lightPipeline =
        [m_state->device newRenderPipelineStateWithDescriptor:light error:&error];
    if (!m_state->lightPipeline) {
        LOG_ERROR("[Metal] Light pipeline failed: %s", [[error localizedDescription] UTF8String]);
        return false;
    }


    // A 1x1 empty mask keeps the light shader valid before any walls exist.
    SetOccluders(nullptr, 0, 0.0f, 0.0f, 1.0f, 1.0f);
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
    m_state->cameraX    = constants.cameraX;
    m_state->cameraY    = constants.cameraY;
    m_state->cameraZoom = constants.zoom;

    [m_state->encoder setRenderPipelineState:m_state->tilePipeline];
    [m_state->encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
    [m_state->encoder setFragmentTexture:m_state->tileIdTex atIndex:0];
    [m_state->encoder setFragmentTexture:m_state->atlasTex atIndex:1];
    [m_state->encoder setFragmentTexture:m_state->paletteTex atIndex:2];
    [m_state->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    // Back to the quad pipeline: DrawQuad assumes it is current.
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

void MetalRenderer::SetOccluders(const Wall* walls, int wallCount,
                                 float originX, float originY,
                                 float worldWidth, float worldHeight)
{
    if (!m_state || !m_state->occluderPipeline) {
        return;
    }

    m_state->maskOriginX = originX;
    m_state->maskOriginY = originY;
    m_state->maskWidth   = std::max(1.0f, worldWidth);
    m_state->maskHeight  = std::max(1.0f, worldHeight);

    // One mask texel per 4 world pixels, capped so huge maps stay bounded.
    const NSUInteger texW = std::clamp<NSUInteger>(
        static_cast<NSUInteger>(m_state->maskWidth / 4.0f), 1, 2048);
    const NSUInteger texH = std::clamp<NSUInteger>(
        static_cast<NSUInteger>(m_state->maskHeight / 4.0f), 1, 2048);

    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                           width:texW
                                                          height:texH
                                                       mipmapped:NO];
    desc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;

    id<MTLTexture> mask = [m_state->device newTextureWithDescriptor:desc];
    if (!mask) {
        LOG_ERROR("[Metal] Occlusion mask allocation failed");
        return;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture     = mask;
    pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLCommandBuffer>        buffer  = [m_state->queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:m_state->occluderPipeline];

    int drawn = 0;
    for (int i = 0; i < wallCount; ++i) {
        const Wall& wall = walls[i];
        if (!wall.blocksLight) {
            continue;
        }
        const float constants[8] = {
            wall.x, wall.y, wall.w, wall.h,
            originX, originY, m_state->maskWidth, m_state->maskHeight,
        };
        [encoder setVertexBytes:constants length:sizeof(constants) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        ++drawn;
    }

    [encoder endEncoding];
    [buffer commit];
    [buffer waitUntilCompleted];

    m_state->occlusionMask = mask;
    LOG_INFO("[Metal] Occlusion mask rebuilt: %lux%lu texels, %d occluder(s)",
             (unsigned long)texW, (unsigned long)texH, drawn);
}

void MetalRenderer::DrawWalls(const Wall* walls, int wallCount)
{
    if (!m_state || !m_state->encoder) {
        return;
    }
    // Walls are ordinary quads once transformed to screen space, so they go
    // through the existing quad path rather than a pipeline of their own.
    for (int i = 0; i < wallCount; ++i) {
        const Wall& wall = walls[i];
        Quad quad;
        quad.x = (wall.x - m_state->cameraX) * m_state->cameraZoom + m_state->viewportWidth * 0.5f;
        quad.y = (wall.y - m_state->cameraY) * m_state->cameraZoom + m_state->viewportHeight * 0.5f;
        quad.w = wall.w * m_state->cameraZoom;
        quad.h = wall.h * m_state->cameraZoom;
        quad.color = wall.color;
        DrawQuad(quad);
    }
}

void MetalRenderer::DrawLighting(const Light* lights, int lightCount,
                                 const TileDrawConstants& camera)
{
    if (!m_state || !m_state->encoder || !m_state->occlusionMask) {
        return;
    }

    // Visible world rectangle, used to cull lights that cannot reach the view.
    const float halfW = camera.viewportW * 0.5f / camera.zoom;
    const float halfH = camera.viewportH * 0.5f / camera.zoom;
    const float viewMinX = camera.cameraX - halfW, viewMaxX = camera.cameraX + halfW;
    const float viewMinY = camera.cameraY - halfH, viewMaxY = camera.cameraY + halfH;

    [m_state->encoder setRenderPipelineState:m_state->lightPipeline];
    [m_state->encoder setFragmentTexture:m_state->occlusionMask atIndex:0];

    int drawn = 0;
    for (int i = 0; i < lightCount; ++i) {
        const Light& light = lights[i];
        const float closestX = std::clamp(light.x, viewMinX, viewMaxX);
        const float closestY = std::clamp(light.y, viewMinY, viewMaxY);
        const float dx = light.x - closestX, dy = light.y - closestY;
        if (dx * dx + dy * dy > light.distance * light.distance) {
            continue;  // beam cannot reach the viewport
        }

        const float halfAngle = light.angleDeg * 0.5f * 3.14159265f / 180.0f;
        const float constants[24] = {
            light.color.r * light.intensity, light.color.g * light.intensity,
            light.color.b * light.intensity, 1.0f,
            m_state->maskOriginX, m_state->maskOriginY,
            m_state->maskWidth, m_state->maskHeight,
            light.x, light.y,
            light.dirX, light.dirY,
            light.distance,
            std::cos(halfAngle),
            light.softness,
            light.mode == LightMode::ScreenSpace ? 1.0f : 0.0f,
            camera.cameraX, camera.cameraY,
            camera.zoom, light.pixelArt ? 1.0f : 0.0f,
            camera.viewportW, camera.viewportH,
            0.0f, 0.0f,
        };
        [m_state->encoder setFragmentBytes:constants length:sizeof(constants) atIndex:0];
        [m_state->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        ++drawn;
    }
    (void)drawn;


    // Leave the quad pipeline bound: DrawQuad assumes it is current.
    [m_state->encoder setRenderPipelineState:m_state->pipeline];
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
    m_state->occluderPipeline = nil;  // ARC releases
    m_state->lightPipeline    = nil;
    m_state->occlusionMask    = nil;
    m_state->tilePipeline = nil;
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
