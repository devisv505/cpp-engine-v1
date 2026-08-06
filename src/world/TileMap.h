#pragma once

#include <cstdint>
#include <vector>

#include "world/TileRegistry.h"

namespace engine {

// Fixed-size tile grid. Storage is a flat row-major uint16 array; the
// renderer mirrors it into a GPU texture, so every mutation goes through
// Set/Fill which accumulate a dirty rectangle for cheap partial uploads.
class TileMap {
public:
    static constexpr int kMaxDimension = 4096;

    bool Create(int width, int height, TileId fill = 0);

    int  Width() const { return m_width; }
    int  Height() const { return m_height; }
    bool InBounds(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < m_width && y < m_height;
    }

    TileId Get(int x, int y) const
    {
        return InBounds(x, y) ? m_tiles[static_cast<size_t>(y) * m_width + x] : 0;
    }

    void Set(int x, int y, TileId id);

    // Paints a filled square of side (2*radius - 1) centered on (x, y),
    // clipped to the map. radius 1 = single tile.
    void PaintBrush(int x, int y, int radius, TileId id);

    const uint16_t* Data() const { return m_tiles.data(); }
    uint16_t*       MutableData() { return m_tiles.data(); }

    // Dirty rectangle since the last TakeDirtyRegion, as [x, y, w, h].
    // Returns false when nothing changed.
    bool TakeDirtyRegion(int& x, int& y, int& w, int& h);
    void MarkAllDirty();

private:
    void MarkDirty(int x0, int y0, int x1, int y1);

    int                   m_width  = 0;
    int                   m_height = 0;
    std::vector<uint16_t> m_tiles;

    bool m_dirty  = false;
    int  m_dirtyX0 = 0, m_dirtyY0 = 0, m_dirtyX1 = 0, m_dirtyY1 = 0;
};

} // namespace engine
