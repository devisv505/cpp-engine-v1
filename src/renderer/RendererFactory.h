#pragma once

#include <memory>

#include <SDL3/SDL_video.h>

#include "renderer/IRenderer.h"

namespace engine {

enum class RendererBackend { D3D12, Vulkan, Metal };

// The backend is fixed at compile time by the ENGINE_BACKEND_* define the
// build system sets for the current platform.
RendererBackend GetActiveBackend();

// Flags the SDL window must be created with for the active backend
// (e.g. SDL_WINDOW_VULKAN). Callable before any renderer exists, because the
// window has to be created first.
SDL_WindowFlags GetRequiredWindowFlags();

std::unique_ptr<IRenderer> CreateRenderer();

} // namespace engine
