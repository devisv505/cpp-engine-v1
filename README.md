# Cpp Engine

[![CI](https://github.com/devisv505/cpp-engine-v1/actions/workflows/ci.yml/badge.svg)](https://github.com/devisv505/cpp-engine-v1/actions/workflows/ci.yml)

A cross-platform **2D engine** in C++20: SDL3 for windowing and events, a renderer
abstraction with a native backend per platform, and **Lua** for defining content.

| Platform | Backend     | Status |
|----------|-------------|--------|
| Windows  | Direct3D 12 | compile-checked in CI |
| Linux    | Vulkan      | compile-checked in CI |
| macOS    | Metal       | built and run-tested |

The C++ side owns the window, the graphics device, the swapchain, and all drawing.
Lua describes *what* to draw. The engine runs `scripts/main.lua` at startup; the script
calls into a controlled `engine` API to set the clear color and add quads, and the
renderer draws the result every frame.

## The Lua API

```lua
engine.log(message)
engine.set_clear_color(r, g, b [, a])
engine.add_quad{ x = , y = , w = , h = , color = {r, g, b [, a]} }
engine.window_size() -> width, height   -- framebuffer pixels
```

Colors are floats in `0..1`. Coordinates are pixels with the origin at the **top-left**,
+X right and +Y down. `color` accepts either `{0.9, 0.3, 0.2}` or `{r = 0.9, g = 0.3, b = 0.2}`.

A minimal scene:

```lua
engine.set_clear_color(0.09, 0.10, 0.13)

local width, height = engine.window_size()
engine.add_quad{
    x = width * 0.25, y = height * 0.25,
    w = width * 0.5,  h = height * 0.5,
    color = { 0.20, 0.60, 0.86 },
}
```

Edit `scripts/main.lua` and press **F5** in the running engine to re-run it — the scene
updates without a restart. Resizing the window also re-runs the script, so layouts
expressed against `engine.window_size()` adapt. **Esc** quits.

Scripts get a deliberately limited standard library (`base`, `string`, `table`, `math` —
no `io`, `os`, or `package`), so the engine is reachable only through the `engine` table.
A script error is logged with a traceback and leaves the engine running.

## How drawing works

Quads carry no vertex buffers. The vertex shader generates the four corners from the
vertex index and a 64-byte per-draw constant block (`QuadConstants` in
[`src/core/Scene2D.h`](src/core/Scene2D.h)), delivered as Metal vertex bytes, Vulkan push
constants, or D3D12 root constants. A quad is therefore one constant upload plus a
four-vertex triangle strip.

The one asymmetry worth knowing: Metal and D3D12 clip space is Y-up, Vulkan's is Y-down,
so the Vulkan shader omits the Y flip the other two apply.

## Layout

```
config/window.json        window settings (engine configuration)
scripts/main.lua          scene definition (content)
shaders/quad.{vert,frag}  GLSL sources, compiled to SPIR-V for the Vulkan backend
third_party/nlohmann/     vendored JSON single header
third_party/lua/          vendored Lua 5.5.1
src/core/                 Log, Config, Window, Scene2D, Application
src/scripting/            ScriptHost — Lua VM, engine API bindings, error isolation
src/renderer/             IRenderer interface + compile-time backend factory
src/renderer/{dx12,vulkan,metal}/   one backend, compiled only on its platform
```

Window settings stay in JSON because they configure the engine; game content lives in Lua.
`config/window.json` accepts `title`, `width`, `height`, `fullscreen`, `resizable`, and
`vsync`. A missing file, invalid JSON, or a wrong-typed value logs a warning and falls back
to defaults, so the app always starts.

## Building

Requires CMake ≥ 3.24 and a C++20 compiler. If SDL3 is not installed, CMake fetches and
builds it from source automatically (the first configure is slow).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
./build/engine
```

### Per-platform prerequisites

- **macOS** — Xcode Command Line Tools; `brew install sdl3` recommended.
  (On Apple Silicon the build pins `arm64` even under a Rosetta CMake.)
- **Linux** — Vulkan headers/loader (`libvulkan-dev` or the LunarG SDK) and a GLSL
  compiler for the shader build step: `glslc` (Vulkan SDK) or `glslangValidator`
  (`glslang-tools`).
- **Windows** — Visual Studio 2022 with the Windows 10/11 SDK.

## CI and releases

Every push and pull request to `main` builds all three platforms on GitHub Actions.
CI builds use `-DENGINE_FETCH_SDL=ON`, which compiles SDL3 from source and links it
statically, so the produced binaries are self-contained.

Publishing a release is tag-driven:

```sh
git tag v0.2.0
git push origin v0.2.0
```

That builds all three platforms, packages each binary with its `config/`, `scripts/`, and
shader data, and publishes them on the
[Releases page](https://github.com/devisv505/cpp-engine-v1/releases).

## Third-party

[SDL3](https://libsdl.org) (zlib), [Lua 5.5.1](https://lua.org) (MIT), and
[nlohmann/json](https://github.com/nlohmann/json) (MIT).
