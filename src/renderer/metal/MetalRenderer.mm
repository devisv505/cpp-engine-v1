#include "renderer/metal/MetalRenderer.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_metal.h>

#include "core/Log.h"
#include "core/Window.h"

namespace engine {

struct MetalState {
    id<MTLDevice>       device = nil;
    id<MTLCommandQueue> queue  = nil;
    SDL_MetalView       view   = nullptr;
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
    LOG_INFO("[Metal] Command queue created");

    m_state->view = SDL_Metal_CreateView(window.GetSDLWindow());
    if (!m_state->view) {
        LOG_ERROR("[Metal] SDL_Metal_CreateView failed: %s", SDL_GetError());
        return false;
    }

    // SDL creates the CAMetalLayer but does not associate a device with it.
    CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(m_state->view);
    if (!layer) {
        LOG_ERROR("[Metal] SDL_Metal_GetLayer returned null");
        return false;
    }
    layer.device = m_state->device;
    LOG_INFO("[Metal] CAMetalLayer attached to device");

    return true;
}

void MetalRenderer::Shutdown()
{
    if (!m_state) {
        return;
    }
    if (m_state->view) {
        SDL_Metal_DestroyView(m_state->view);
        m_state->view = nullptr;
    }
    m_state->queue  = nil;  // ARC releases
    m_state->device = nil;
    delete m_state;
    m_state = nullptr;
}

} // namespace engine
