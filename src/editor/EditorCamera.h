#pragma once


#include "world/WorldConfig.h"

namespace engine {

// Factorio-style 2D camera in world-pixel space. Zoom is "screen pixels per
// world pixel" and approaches its target exponentially, anchored on the mouse
// cursor so the world point under it stays put. Middle-mouse drag pins the
// grabbed world point directly under the cursor with no smoothing.
class EditorCamera {
public:
    void Configure(const EditorConfig& config, int mapWidthPx, int mapHeightPx);

    // dt in seconds; mouse position in framebuffer pixels.
    void Update(float dt, float mouseX, float mouseY, int viewportW, int viewportH);

    void OnMouseWheel(float wheelY, float mouseX, float mouseY);
    void BeginDrag(float mouseX, float mouseY);
    void EndDrag() { m_dragging = false; }
    bool IsDragging() const { return m_dragging; }

    // Keyboard pan state, fed by the application from key events.
    void SetPanInput(float dirX, float dirY) { m_panX = dirX; m_panY = dirY; }

    void   ScreenToWorld(float sx, float sy, float& wx, float& wy) const;
    float  Zoom() const { return m_zoom; }
    float  X() const { return m_x; }
    float  Y() const { return m_y; }
    void   CenterOn(float wx, float wy) { m_x = wx; m_y = wy; }


private:
    float m_x = 0.0f, m_y = 0.0f;   // world pixels at viewport center
    float m_zoom       = 1.0f;
    float m_targetZoom = 1.0f;
    float m_zoomMin = 0.125f, m_zoomMax = 8.0f;
    float m_panSpeed = 900.0f;

    float m_panX = 0.0f, m_panY = 0.0f;

    bool  m_dragging = false;
    float m_dragWorldX = 0.0f, m_dragWorldY = 0.0f;

    int m_viewportW = 1, m_viewportH = 1;
};

} // namespace engine
