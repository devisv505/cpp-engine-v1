#pragma once

namespace engine {

    // An axis-aligned rectangle: top-left corner plus size, in whatever units
    // the caller works in (screen pixels for quads, world pixels for bounds).
    // Pure geometry with no colour or draw state, so it is reusable for culling,
    // hit-testing and bounds.
    struct Rect {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
    };

} // namespace engine
