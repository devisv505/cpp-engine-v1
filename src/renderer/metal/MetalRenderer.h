#pragma once

#include "renderer/IRenderer.h"

namespace engine {

// Objective-C objects live behind this pimpl (defined in MetalRenderer.mm),
// keeping this header includable from plain C++.
struct MetalState;

class MetalRenderer final : public IRenderer {
public:
    ~MetalRenderer() override;

    bool Init(Window& window) override;
    void Shutdown() override;
    const char* GetBackendName() const override { return "Metal"; }

private:
    MetalState* m_state = nullptr;
};

} // namespace engine
