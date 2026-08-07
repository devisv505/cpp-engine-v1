#pragma once

#include <cstddef>
#include <functional>

#include "core/math/Vector2.h"

namespace engine {

    // 2D integer vector, for tile and grid coordinates.
    struct Vector2Int {
        int x = 0;
        int y = 0;

        // Exact by nature, unlike the float vector, so equality is well defined.
        [[nodiscard]] constexpr bool operator==(const Vector2Int&) const = default;
    };

    // --- Arithmetic ------------------------------------------------------------
    [[nodiscard]] constexpr Vector2Int operator+(const Vector2Int& a, const Vector2Int& b)
    {
        return {a.x + b.x, a.y + b.y};
    }

    [[nodiscard]] constexpr Vector2Int operator-(const Vector2Int& a, const Vector2Int& b)
    {
        return {a.x - b.x, a.y - b.y};
    }

    [[nodiscard]] constexpr Vector2Int operator*(const Vector2Int& v, int scalar)
    {
        return {v.x * scalar, v.y * scalar};
    }

    [[nodiscard]] constexpr Vector2Int operator*(int scalar, const Vector2Int& v)
    {
        return {v.x * scalar, v.y * scalar};
    }

    // Truncates toward zero, like built-in integer division. For grid coordinates
    // that can go negative, prefer FloorDiv — see the note below.
    [[nodiscard]] constexpr Vector2Int operator/(const Vector2Int& v, int scalar)
    {
        return {v.x / scalar, v.y / scalar};
    }

    [[nodiscard]] constexpr Vector2Int operator-(const Vector2Int& v)
    {
        return {-v.x, -v.y};
    }

    constexpr Vector2Int& operator+=(Vector2Int& a, const Vector2Int& b)
    {
        a.x += b.x;
        a.y += b.y;
        return a;
    }

    constexpr Vector2Int& operator-=(Vector2Int& a, const Vector2Int& b)
    {
        a.x -= b.x;
        a.y -= b.y;
        return a;
    }

    constexpr Vector2Int& operator*=(Vector2Int& v, int scalar)
    {
        v.x *= scalar;
        v.y *= scalar;
        return v;
    }

    constexpr Vector2Int& operator/=(Vector2Int& v, int scalar)
    {
        v.x /= scalar;
        v.y /= scalar;
        return v;
    }

    // --- Grid helpers ----------------------------------------------------------
    // C++ integer division truncates toward zero, so -1 / 32 == 0 even though the
    // cell containing -1 is cell -1. Shaders use floor(), so plain `/` would make
    // C++ and GLSL/MSL/HLSL disagree for anything left of or above the origin.
    // These two agree with floor() everywhere.

    [[nodiscard]] constexpr int FloorDiv(int value, int divisor)
    {
        const int quotient  = value / divisor;
        const int remainder = value % divisor;
        return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? quotient - 1 : quotient;
    }

    [[nodiscard]] constexpr int FloorMod(int value, int divisor)
    {
        const int remainder = value % divisor;
        return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? remainder + divisor
                                                                     : remainder;
    }

    [[nodiscard]] constexpr Vector2Int FloorDiv(const Vector2Int& v, int divisor)
    {
        return {FloorDiv(v.x, divisor), FloorDiv(v.y, divisor)};
    }

    [[nodiscard]] constexpr Vector2Int FloorMod(const Vector2Int& v, int divisor)
    {
        return {FloorMod(v.x, divisor), FloorMod(v.y, divisor)};
    }

    [[nodiscard]] constexpr Vector2Int Min(const Vector2Int& a, const Vector2Int& b)
    {
        return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y};
    }

    [[nodiscard]] constexpr Vector2Int Max(const Vector2Int& a, const Vector2Int& b)
    {
        return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y};
    }

    [[nodiscard]] constexpr Vector2Int Clamp(const Vector2Int& v, const Vector2Int& low,
                                             const Vector2Int& high)
    {
        return Min(Max(v, low), high);
    }

    // --- Conversions -----------------------------------------------------------
    // Widening is exact and free; narrowing is always named after its rounding
    // rule, so no conversion can silently pick truncation.

    [[nodiscard]] constexpr Vector2 ToVector2(const Vector2Int& v)
    {
        return {static_cast<float>(v.x), static_cast<float>(v.y)};
    }

    // The tile containing a world position — matches floor() in the shaders.
    [[nodiscard]] inline Vector2Int FloorToInt(const Vector2& v)
    {
        return {static_cast<int>(std::floor(v.x)), static_cast<int>(std::floor(v.y))};
    }

    [[nodiscard]] inline Vector2Int RoundToInt(const Vector2& v)
    {
        return {static_cast<int>(std::lround(v.x)), static_cast<int>(std::lround(v.y))};
    }

} // namespace engine

// Lets Vector2Int key unordered_map/set — useful for tile lookups and
// dirty-region tracking. The shift keeps mirrored coordinates from colliding.
template <>
struct std::hash<engine::Vector2Int> {
    [[nodiscard]] std::size_t operator()(const engine::Vector2Int& v) const noexcept
    {
        const auto hx = static_cast<std::size_t>(static_cast<unsigned>(v.x));
        const auto hy = static_cast<std::size_t>(static_cast<unsigned>(v.y));
        return hx ^ (hy + 0x9e3779b97f4a7c15ULL + (hx << 6) + (hx >> 2));
    }
};
