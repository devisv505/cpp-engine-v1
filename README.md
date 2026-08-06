# Cpp Engine

Cross-platform C++20 engine skeleton: SDL3 for windowing and events, with a
renderer abstraction whose backend is chosen per platform at compile time.

| Platform | Backend     | Status |
|----------|-------------|--------|
| Windows  | Direct3D 12 | device init written, not yet CI-verified |
| Linux    | Vulkan      | device init written, not yet CI-verified |
| macOS    | Metal       | device init built and tested |

## Current boundary: no rendering yet

Each backend performs **real device initialization** and stops there:

- **Metal** — `MTLDevice`, `MTLCommandQueue`, `CAMetalLayer` attached to the
  SDL window via `SDL_Metal_CreateView`.
- **Vulkan** — `VkInstance` (with SDL-required extensions, validation layer in
  debug builds when available), `VkSurfaceKHR`, physical-device selection,
  `VkDevice` + graphics queue (`VK_KHR_swapchain` enabled for later).
- **Direct3D 12** — debug layer in debug builds, `IDXGIFactory6`,
  high-performance hardware adapter, `ID3D12Device`, direct command queue.

No swapchains, no command recording, no drawing. `IRenderer::BeginFrame()` /
`EndFrame()` are no-op placeholders for that future work.

## Layout

```
config/window.json        runtime window settings (copied next to the binary)
third_party/nlohmann/     vendored JSON single header (v3.12.0)
src/core/                 Log, Config (JSON loader), Window (SDL RAII), Application
src/renderer/             IRenderer interface + compile-time backend factory
src/renderer/dx12|vulkan|metal/   one backend, compiled only on its platform
```

The build defines exactly one of `ENGINE_BACKEND_D3D12` / `ENGINE_BACKEND_VULKAN`
/ `ENGINE_BACKEND_METAL`; `RendererFactory.cpp` selects the implementation and
reports the SDL window flags the backend requires (e.g. `SDL_WINDOW_VULKAN`)
before the window is created.

## Window configuration

`config/window.json` is read at startup, resolved relative to the executable
(`SDL_GetBasePath`), so launching from any directory works:

```json
{
  "window": {
    "title": "Cpp Engine",
    "width": 1280,
    "height": 720,
    "fullscreen": false,
    "resizable": true,
    "vsync": true
  }
}
```

| Key | Type | Default | Notes |
|-----|------|---------|-------|
| `title` | string | `"Cpp Engine"` | window title |
| `width` / `height` | int | `1280` / `720` | clamped to ≥ 1 |
| `fullscreen` | bool | `false` | borderless desktop fullscreen |
| `resizable` | bool | `true` | |
| `vsync` | bool | `true` | stored now, consumed at swapchain creation later |

A missing file, invalid JSON, or wrong-typed value logs a warning and falls
back to defaults — the app always starts.

## Building

Requires CMake ≥ 3.24 and a C++20 compiler. If SDL3 is not installed,
CMake fetches and builds it from source automatically (first configure is slow).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
./build/engine
```

### Per-platform prerequisites

- **macOS** — Xcode Command Line Tools; `brew install sdl3` recommended.
  (On Apple Silicon the build pins `arm64` even under a Rosetta CMake.)
- **Linux** — Vulkan headers/loader (`libvulkan-dev` or the LunarG SDK);
  SDL3 from your package manager, or let FetchContent build it.
- **Windows** — Visual Studio 2022 with the Windows 10/11 SDK
  (provides `d3d12.lib`, `dxgi.lib`, `dxguid.lib`).

Expected startup log (macOS example):

```
Window config from .../build/config/window.json: "Cpp Engine" 1280x720 ...
Renderer backend: Metal
Window created: "Cpp Engine" 1280x720 resizable
[Metal] Device: Apple M5 Pro
[Metal] Command queue created
[Metal] CAMetalLayer attached to device
```

The Vulkan and D3D12 paths follow standard initialization patterns but have
only been verified structurally on macOS; a CI matrix (windows-latest MSVC,
ubuntu-latest + Vulkan SDK) is the natural next step to compile-check them.
