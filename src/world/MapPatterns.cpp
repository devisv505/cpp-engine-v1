#include "world/MapPatterns.h"

#include <algorithm>
#include <random>

#include "core/Log.h"

namespace engine {

namespace {

TileId TileAt(const PatternParams& params, size_t index)
{
    return params.tiles.empty() ? 0 : params.tiles[index % params.tiles.size()];
}

void FillCheckerboard(TileMap& map, const PatternParams& params)
{
    const int cell = std::max(1, params.cellSize);
    const size_t count = std::max<size_t>(1, params.tiles.size());
    for (int y = 0; y < map.Height(); ++y) {
        for (int x = 0; x < map.Width(); ++x) {
            const size_t index = static_cast<size_t>((x / cell) + (y / cell)) % count;
            map.Set(x, y, TileAt(params, index));
        }
    }
}

void FillRandom(TileMap& map, const PatternParams& params)
{
    // A fixed default seed keeps generation reproducible run to run.
    std::mt19937 rng(params.seed != 0 ? params.seed : 0x2D2816FEu);

    std::vector<float> weights = params.weights;
    weights.resize(std::max<size_t>(1, params.tiles.size()), 1.0f);
    std::discrete_distribution<size_t> pick(weights.begin(), weights.end());

    for (int y = 0; y < map.Height(); ++y) {
        for (int x = 0; x < map.Width(); ++x) {
            map.Set(x, y, TileAt(params, pick(rng)));
        }
    }
}

void FillSolid(TileMap& map, const PatternParams& params)
{
    const TileId id = TileAt(params, 0);
    for (int y = 0; y < map.Height(); ++y) {
        for (int x = 0; x < map.Width(); ++x) {
            map.Set(x, y, id);
        }
    }
}

} // namespace

bool GenerateMapPattern(TileMap& map, const std::string& pattern, const PatternParams& params)
{
    if (pattern == "checkerboard") {
        FillCheckerboard(map, params);
    } else if (pattern == "random") {
        FillRandom(map, params);
    } else if (pattern == "solid") {
        FillSolid(map, params);
    } else {
        LOG_ERROR("Unknown map pattern '%s'", pattern.c_str());
        return false;
    }
    LOG_INFO("Generated %dx%d map with pattern '%s' (%zu tile type(s))",
             map.Width(), map.Height(), pattern.c_str(), params.tiles.size());
    return true;
}

const std::vector<std::string>& KnownPatternNames()
{
    static const std::vector<std::string> kNames = {"checkerboard", "random", "solid"};
    return kNames;
}

} // namespace engine
