# Engine505

[![CI](https://github.com/devisv505/cpp-engine-v1/actions/workflows/ci.yml/badge.svg)](https://github.com/devisv505/cpp-engine-v1/actions/workflows/ci.yml)

A cross-platform **2D tile-map engine** in C++20: SDL3 for windowing and events, a
renderer abstraction with a native backend per platform, **Lua** for defining content,
rendering a tile map the engine boots into fullscreen.

| Platform | Backend     | Status |
|----------|-------------|--------|
| Windows  | Direct3D 12 | compile-checked in CI |
| Linux    | Vulkan      | compile-checked in CI |
| macOS    | Metal       | built and run-tested |

Factorio's division of labor: Lua defines tile types and picks how the map is
generated; C++ owns generation, storage, rendering, editing, and everything
performance-critical. The whole visible map renders as **one draw call** — a
fullscreen pass reads a tile-id texture, so pan/zoom cost is independent of map
size, and painting uploads only the dirty rectangle.

## Controls

The engine starts fullscreen (set `fullscreen` in `config/window.json` to run
windowed) showing the Lua-configured map.

- **MMB drag** move the map · **wheel** zoom centered on the cursor ·
  **WASD** pan (configurable) · **F5** re-run the script and regenerate ·
  **Esc** quit

There is no UI layer yet — tile painting and map save/load exist in the engine
(`src/world/MapIO.h`) but have no controls bound until a custom UI lands.

## The Lua configuration

`scripts/main.lua` runs once at startup (and on F5). It defines tile types and
selects a generation pattern through plain globals:

```lua
tiles = {
    { name = "slate", color = {0.35, 0.38, 0.42, 1.0} },                 -- solid color
    { name = "sand",  texture = "textures/sand.png" },                   -- texture
    { name = "moss",  texture = "textures/moss.png",
      tint = {0.8, 1.0, 0.8, 1.0} },                                     -- texture x tint
    { name = "water", color = {0.16, 0.32, 0.50, 1.0}, walkable = false },
}

map = {
    pattern = "checkerboard",          -- or "random", "solid" (implemented in C++)
    colors = {                          -- shorthand: auto-defines tile types
        { 0.16, 0.16, 0.16, 1.0 },
        { 0.25, 0.25, 0.25, 1.0 }
    },
    -- or: tiles = {"slate", "moss"}, plus width, height, cell_size, seed, weights
}

editor = {
    pan_speed = 900, zoom_min = 0.125, zoom_max = 8.0,
    keys = { up = "W", down = "S", left = "A", right = "D" },
}
```

Lua never draws tiles, generates maps, or moves the camera — it only configures;
the patterns and every per-tile operation run in C++. The immediate-mode quad API
(`engine.set_clear_color`, `engine.add_quad{...}`, `engine.window_size`,
`engine.log`) is still available for overlays.

Scripts get a deliberately limited standard library (`base`, `string`, `table`, `math` —
no `io`, `os`, or `package`), so the engine is reachable only through its API.
A script error is logged with a traceback and leaves the engine running.

## How drawing works

The tile pass is a single fullscreen draw: each fragment computes its world tile
from the camera constants, fetches the id from an `R16Uint` map texture, and
resolves the tile's color or atlas texture (with tint) through a palette texture.
Tile textures are packed into one atlas at load by `stb_image` + a shelf packer.

Quads carry no vertex buffers — the vertex shader generates corners from the
vertex index and a 64-byte constant block ([`src/core/Scene2D.h`](src/core/Scene2D.h)),
delivered as Metal vertex bytes, Vulkan push constants, or D3D12 root constants.
One asymmetry worth knowing: Metal and D3D12 clip space is Y-up, Vulkan's is
Y-down, so the Vulkan quad shader omits the Y flip the other two apply.

## Layout

```
config/window.json        window settings (engine configuration)
scripts/main.lua          world + editor configuration (content)
shaders/                  GLSL sources, compiled to SPIR-V for the Vulkan backend
third_party/              vendored Lua 5.5.1, nlohmann/json, stb_image
src/core/                 Log, Config, Window, Scene2D, Application
src/scripting/            ScriptHost — Lua VM, engine API bindings, error isolation
src/world/                TileRegistry, TileMap, patterns, atlas builder, map IO
src/editor/               EditorCamera (pan, zoom, drag)
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
