#pragma once

#include <vector>

#include "core/Scene2D.h"

namespace engine {

// Walls and lights that sit on top of the tile map. Lua describes them in
// tile units; everything here is already converted to world pixels.

struct Wall {
    float x = 0.0f, y = 0.0f;   // top-left corner, world pixels
    float w = 0.0f, h = 0.0f;
    Color color;
    bool  blocksLight = true;
    bool  collision   = true;   // stored for future entity collision
};

enum class LightMode {
    VolumetricCone,   // cheap: analytic cone, raymarched against the occlusion mask
    ScreenSpace,      // omnidirectional light with mask-cast volumetric shafts
};

struct Light {
    float     x = 0.0f, y = 0.0f;        // world pixels
    float     dirX = 1.0f, dirY = 0.0f;  // normalized
    LightMode mode      = LightMode::VolumetricCone;
    Color     color{1.0f, 1.0f, 1.0f, 1.0f};
    float     intensity = 1.0f;
    float     distance  = 320.0f;        // beam length, world pixels
    float     angleDeg  = 35.0f;         // full cone angle
    float     softness  = 0.25f;         // 0 = hard edge, 1 = fully feathered
    bool      pixelArt  = false;         // world-aligned blocks and discrete light levels
};

struct Environment {
    std::vector<Wall> walls;
    std::vector<Light> lights;

    void Clear()
    {
        walls.clear();
        lights.clear();
    }
};

} // namespace engine
