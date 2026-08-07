#pragma once

namespace engine {

    // Linear RGBA, components normally in 0..1. An aggregate, like the vectors, so
    // `Color{1.0f, 0.5f, 0.0f}` and designated initializers both work.
    struct Color {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;
    };

} // namespace engine
