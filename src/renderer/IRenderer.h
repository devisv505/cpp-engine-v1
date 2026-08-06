#pragma once

#include "core/Scene2D.h"

namespace engine {

class Window;

// 2D renderer abstraction. Exactly one backend implementation is compiled per
// platform: Direct3D 12 on Windows, Vulkan on Linux, Metal on macOS.
//
// Quads carry no vertex buffers: the vertex shader generates the four corners
// from the vertex index and the per-draw QuadConstants, so a draw is a single
// constant upload plus a 4-vertex triangle strip.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Creates device objects and the swapchain for an already-created window.
    // Returns false on failure; the app aborts startup.
    virtual bool Init(Window& window) = 0;

    // Releases all backend objects. Safe to call more than once.
    virtual void Shutdown() = 0;

    // Acquires the next frame's target and clears it. A frame that fails to
    // acquire is skipped: DrawQuad and EndFrame become no-ops until the next one.
    virtual void BeginFrame(const Color& clearColor) = 0;

    // Draws one quad in pixel coordinates. Only valid between Begin/EndFrame.
    virtual void DrawQuad(const Quad& quad) = 0;

    // Finishes and presents the frame.
    virtual void EndFrame() = 0;

    // Framebuffer size changed (window resize, display scale change).
    virtual void OnResize(int pixelWidth, int pixelHeight) = 0;

    virtual const char* GetBackendName() const = 0;
};

} // namespace engine
