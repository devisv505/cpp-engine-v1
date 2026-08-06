#pragma once

#include <vector>

namespace engine {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// A rectangle in 2D pixel space: origin is the top-left of the window,
// +X points right, +Y points down. Backends convert to their own clip space.
struct Quad {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    Color color;
};

// The drawable contents of a frame. Built by Lua, consumed by the renderer.
struct Scene2D {
    Color             clearColor{0.08f, 0.09f, 0.12f, 1.0f};
    std::vector<Quad> quads;

    void Reset()
    {
        clearColor = Color{0.08f, 0.09f, 0.12f, 1.0f};
        quads.clear();
    }
};

// Per-draw constants handed to every backend's quad shader. The layout is
// shared by MSL, HLSL, and GLSL: 64 bytes, small enough for push/root constants.
struct QuadConstants {
    float rect[4];      // x, y, w, h in pixels
    float color[4];     // rgba
    float viewport[2];  // framebuffer width, height in pixels
    float padding[2];
};

} // namespace engine
