#include "TestSupport.h"

#include <type_traits>
#include <unordered_map>

#include "core/Scene2D.h"
#include "core/math/Vector2Int.h"

using namespace engine;
using tests::TestRun;

int main()
{
    TestRun t("math");

    constexpr Vector2 a{3.0f, 4.0f};
    constexpr Vector2 b{.x = 1.0f, .y = 2.0f};  // designated init must keep working

    static_assert(LengthSquared(a) == 25.0f);
    static_assert(Dot(a, b) == 11.0f);
    static_assert((a + b).x == 4.0f && (a - b).y == 2.0f);
    static_assert((a * 2.0f).x == 6.0f && (2.0f * a).x == 6.0f);
    static_assert((a / 2.0f).y == 2.0f);
    static_assert((-a).x == -3.0f);
    static_assert((a * b).x == 3.0f);  // component-wise
    static_assert(Lerp(Vector2{0, 0}, Vector2{10, 10}, 0.5f).x == 5.0f);
    static_assert(ApproxEquals(Vector2{1.0f, 1.0f}, Vector2{1.0f, 1.0f}));
    static_assert(DistanceSquared(Vector2{0, 0}, Vector2{3, 4}) == 25.0f);
    static_assert(Clamp(Vector2{5, -5}, Vector2{0, 0}, Vector2{1, 1}).y == 0.0f);
    CHECK(t, "Vector2 constexpr arithmetic", true);

    Vector2 m{1, 1};
    m += a; m -= b; m *= 2.0f; m /= 2.0f;
    CHECK(t, "Vector2 compound assignment", m.x == 3.0f && m.y == 3.0f);
    CHECK(t, "Length(3,4) is 5", Length(a) == 5.0f);
    CHECK(t, "Normalized(0,0) returns the fallback, not NaN",
          Normalized(Vector2{0, 0}).x == 0.0f && Normalized(Vector2{0, 0}).y == 0.0f);

    static_assert(Vector2Int{2, 3} == Vector2Int{2, 3});
    static_assert(Vector2Int{2, 3} != Vector2Int{2, 4});
    static_assert((Vector2Int{-1, 33} * 2).y == 66 && (2 * Vector2Int{-1, 33}).y == 66);
    CHECK(t, "Vector2Int equality and scaling", true);

    // The tile-coordinate trap: built-in division truncates toward zero, but
    // the cell containing -1 is -1, and the shaders use floor().
    static_assert((-1 / 32) == 0);
    static_assert(FloorDiv(-1, 32) == -1);
    static_assert(FloorDiv(-33, 32) == -2);
    static_assert(FloorMod(-1, 32) == 31);
    static_assert(FloorDiv(Vector2Int{-1, -33}, 32) == (Vector2Int{-1, -2}));
    CHECK(t, "FloorDiv/FloorMod agree with shader floor() on negatives", true);

    static_assert(ToVector2(Vector2Int{2, 3}).x == 2.0f);
    CHECK(t, "FloorToInt rounds down", FloorToInt(Vector2{-0.5f, 1.9f}) == (Vector2Int{-1, 1}));
    CHECK(t, "RoundToInt rounds to nearest", RoundToInt(Vector2{-0.5f, 1.9f}) == (Vector2Int{-1, 2}));

    std::unordered_map<Vector2Int, int> grid;
    grid[{1, 2}] = 7;
    grid[{2, 1}] = 9;
    CHECK(t, "Vector2Int hashes: mirrored keys do not collide", grid.size() == 2);

    // Quad derives from Rect so quad.x keeps working; a stray virtual would
    // silently change every sizeof-based constant upload.
    static_assert(std::is_aggregate_v<Quad>);
    static_assert(std::is_trivially_copyable_v<Quad>);
    static_assert(!std::is_polymorphic_v<Quad>);
    static_assert(sizeof(Rect) == 16 && sizeof(Quad) == 32);
    Quad q;
    q.x = 1.0f; q.w = 3.0f;                       // inherited from Rect
    q.color = Color{1.0f, 0.5f, 0.0f, 1.0f};
    const Quad braced{{10.0f, 20.0f, 30.0f, 40.0f}, Color{0, 1, 0, 1}};
    const Rect& asRect = braced;
    CHECK(t, "Quad is a Rect + Color, still an aggregate",
          q.x == 1.0f && asRect.x == 10.0f);

    return t.Result();
}
