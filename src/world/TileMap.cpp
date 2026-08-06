#include "world/TileMap.h"

#include <algorithm>

#include "core/Log.h"

namespace engine {

bool TileMap::Create(int width, int height, TileId fill)
{
    if (width < 1 || height < 1 || width > kMaxDimension || height > kMaxDimension) {
        LOG_ERROR("TileMap: invalid size %dx%d (limit %d)", width, height, kMaxDimension);
        return false;
    }
    m_width  = width;
    m_height = height;
    m_tiles.assign(static_cast<size_t>(width) * height, fill);
    MarkAllDirty();
    return true;
}

void TileMap::Set(int x, int y, TileId id)
{
    if (!InBounds(x, y)) {
        return;
    }
    uint16_t& cell = m_tiles[static_cast<size_t>(y) * m_width + x];
    if (cell == id) {
        return;
    }
    cell = id;
    MarkDirty(x, y, x, y);
}

void TileMap::PaintBrush(int x, int y, int radius, TileId id)
{
    const int r  = std::max(0, radius - 1);
    const int x0 = std::max(0, x - r);
    const int y0 = std::max(0, y - r);
    const int x1 = std::min(m_width - 1, x + r);
    const int y1 = std::min(m_height - 1, y + r);
    if (x0 > x1 || y0 > y1) {
        return;
    }

    bool changed = false;
    for (int ty = y0; ty <= y1; ++ty) {
        uint16_t* row = &m_tiles[static_cast<size_t>(ty) * m_width];
        for (int tx = x0; tx <= x1; ++tx) {
            if (row[tx] != id) {
                row[tx]  = id;
                changed = true;
            }
        }
    }
    if (changed) {
        MarkDirty(x0, y0, x1, y1);
    }
}

bool TileMap::TakeDirtyRegion(int& x, int& y, int& w, int& h)
{
    if (!m_dirty) {
        return false;
    }
    x = m_dirtyX0;
    y = m_dirtyY0;
    w = m_dirtyX1 - m_dirtyX0 + 1;
    h = m_dirtyY1 - m_dirtyY0 + 1;
    m_dirty = false;
    return true;
}

void TileMap::MarkAllDirty()
{
    if (m_width > 0) {
        m_dirty   = true;
        m_dirtyX0 = 0;
        m_dirtyY0 = 0;
        m_dirtyX1 = m_width - 1;
        m_dirtyY1 = m_height - 1;
    }
}

void TileMap::MarkDirty(int x0, int y0, int x1, int y1)
{
    if (!m_dirty) {
        m_dirty   = true;
        m_dirtyX0 = x0;
        m_dirtyY0 = y0;
        m_dirtyX1 = x1;
        m_dirtyY1 = y1;
        return;
    }
    m_dirtyX0 = std::min(m_dirtyX0, x0);
    m_dirtyY0 = std::min(m_dirtyY0, y0);
    m_dirtyX1 = std::max(m_dirtyX1, x1);
    m_dirtyY1 = std::max(m_dirtyY1, y1);
}

} // namespace engine
