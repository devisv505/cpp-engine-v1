#pragma once

namespace engine {

class Window;

// Renderer abstraction. Exactly one backend implementation is compiled per
// platform: Direct3D 12 on Windows, Vulkan on Linux, Metal on macOS.
//
// The skeleton stops at real device initialization (device + command queue +
// window-surface connection). BeginFrame/EndFrame stay no-ops until swapchain
// creation and command recording are implemented.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Creates the backend's device objects against an already-created window.
    // Returns false on failure; the app aborts startup.
    virtual bool Init(Window& window) = 0;

    // Releases all backend objects. Safe to call more than once.
    virtual void Shutdown() = 0;

    virtual void BeginFrame() {}
    virtual void EndFrame() {}

    virtual const char* GetBackendName() const = 0;
};

} // namespace engine
