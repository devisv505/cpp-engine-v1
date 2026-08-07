#pragma once

#include <cmath>

namespace engine {

    // 2D float vector. Deliberately an aggregate: `Vector2 v{1.0f, 2.0f}` and
    // designated initializers keep working, and it stays trivially copyable.
    struct Vector2 {
        float x = 0.0f;
        float y = 0.0f;
    };

    // --- Arithmetic ------------------------------------------------------------

    [[nodiscard]] constexpr Vector2 operator+(const Vector2& a, const Vector2& b)
    {
        return {a.x + b.x, a.y + b.y};
    }

    [[nodiscard]] constexpr Vector2 operator-(const Vector2& a, const Vector2& b)
    {
        return {a.x - b.x, a.y - b.y};
    }

    // Component-wise, matching how shaders multiply a vector by a scale.
    [[nodiscard]] constexpr Vector2 operator*(const Vector2& a, const Vector2& b)
    {
        return {a.x * b.x, a.y * b.y};
    }

    [[nodiscard]] constexpr Vector2 operator*(const Vector2& v, float scalar)
    {
        return {v.x * scalar, v.y * scalar};
    }

    [[nodiscard]] constexpr Vector2 operator*(float scalar, const Vector2& v)
    {
        return {v.x * scalar, v.y * scalar};
    }

    [[nodiscard]] constexpr Vector2 operator/(const Vector2& v, float scalar)
    {
        return {v.x / scalar, v.y / scalar};
    }

    [[nodiscard]] constexpr Vector2 operator-(const Vector2& v)
    {
        return {-v.x, -v.y};
    }

    constexpr Vector2& operator+=(Vector2& a, const Vector2& b)
    {
        a.x += b.x;
        a.y += b.y;
        return a;
    }

    constexpr Vector2& operator-=(Vector2& a, const Vector2& b)
    {
        a.x -= b.x;
        a.y -= b.y;
        return a;
    }

    constexpr Vector2& operator*=(Vector2& v, float scalar)
    {
        v.x *= scalar;
        v.y *= scalar;
        return v;
    }

    constexpr Vector2& operator/=(Vector2& v, float scalar)
    {
        v.x /= scalar;
        v.y /= scalar;
        return v;
    }

    // --- Comparison ------------------------------------------------------------
    // No operator== on purpose: exact float equality is almost never what a caller
    // means, so comparing has to be a deliberate act with a stated tolerance.

    [[nodiscard]] constexpr bool ApproxEquals(const Vector2& a, const Vector2& b, float epsilon = 1e-5f)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return (dx < 0.0f ? -dx : dx) <= epsilon && (dy < 0.0f ? -dy : dy) <= epsilon;
    }

    // --- Geometry --------------------------------------------------------------

    [[nodiscard]] constexpr float Dot(const Vector2& a, const Vector2& b)
    {
        return a.x * b.x + a.y * b.y;
    }

    [[nodiscard]] constexpr float LengthSquared(const Vector2& v)
    {
        return v.x * v.x + v.y * v.y;
    }

    [[nodiscard]] inline float Length(const Vector2& v)
    {
        return std::sqrt(LengthSquared(v));
    }

    [[nodiscard]] constexpr float DistanceSquared(const Vector2& a, const Vector2& b)
    {
        return LengthSquared(b - a);
    }

    [[nodiscard]] inline float Distance(const Vector2& a, const Vector2& b)
    {
        return Length(b - a);
    }

    // Returns `fallback` for a zero-length vector rather than producing NaN, so a
    // degenerate direction cannot silently poison everything downstream.
    [[nodiscard]] inline Vector2 Normalized(const Vector2& v, const Vector2& fallback = {})
    {
        const float lengthSquared = LengthSquared(v);
        if (lengthSquared <= 1e-12f) {
            return fallback;
        }
        return v / std::sqrt(lengthSquared);
    }

    [[nodiscard]] constexpr Vector2 Lerp(const Vector2& a, const Vector2& b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    }

    [[nodiscard]] constexpr Vector2 Min(const Vector2& a, const Vector2& b)
    {
        return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y};
    }

    [[nodiscard]] constexpr Vector2 Max(const Vector2& a, const Vector2& b)
    {
        return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y};
    }

    [[nodiscard]] constexpr Vector2 Clamp(const Vector2& v, const Vector2& low,const Vector2& high)
    {
        return Min(Max(v, low), high);
    }

} // namespace engine
