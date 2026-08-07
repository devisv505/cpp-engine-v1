#pragma once

#include <vector>

#include "core/Quad.h"
#include "core/math/Color.h"

namespace engine {

    // The drawable contents of a frame: a background colour plus a flat list of
    // quads. Rebuilt from scratch by the script on every run (see
    // ScriptHost::RunFile), then consumed by the renderer.
    //
    // Not a scene graph: no hierarchy, transforms or ordering beyond insertion.
    struct Scene2D {
        Color             clearColor{0.08f, 0.09f, 0.12f, 1.0f};
        std::vector<Quad> quads;

        void Reset()
        {
            clearColor = Color{0.08f, 0.09f, 0.12f, 1.0f};
            quads.clear();
        }
    };

} // namespace engine
