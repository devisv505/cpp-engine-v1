#pragma once

#include "core/math/Color.h"
#include "core/math/Rect.h"

namespace engine {

    // A solid-coloured rectangle in 2D pixel space: origin is the top-left of
    // the window, +X points right, +Y points down. Backends convert to their own
    // clip space.
    //
    // Derives from Rect rather than holding one so `quad.x` stays valid: Quad is
    // a Rect that also carries a colour. It remains an aggregate, so
    // `Quad q; q.x = ...;` and `Quad{{x, y, w, h}, color}` both work.
    struct Quad : Rect {
        Color color;
    };

} // namespace engine
