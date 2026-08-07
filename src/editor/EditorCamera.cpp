#include "editor/EditorCamera.h"

#include <algorithm>
#include <cmath>


#include "core/Log.h"

namespace engine {

namespace {


} // namespace

void EditorCamera::Configure(const EditorConfig& config, int mapWidthPx, int mapHeightPx)
{
    m_panSpeed = config.panSpeed;
    m_zoomMin  = std::max(0.01f, config.zoomMin);
    m_zoomMax  = std::max(m_zoomMin, config.zoomMax);

    m_x = mapWidthPx * 0.5f;
    m_y = mapHeightPx * 0.5f;
    m_zoom = m_targetZoom = 1.0f;
}

void EditorCamera::ScreenToWorld(float sx, float sy, float& wx, float& wy) const
{
    wx = m_x + (sx - m_viewportW * 0.5f) / m_zoom;
    wy = m_y + (sy - m_viewportH * 0.5f) / m_zoom;
}

void EditorCamera::OnMouseWheel(float wheelY)
{
    // Each notch scales the target; Update eases the live zoom towards it.
    m_targetZoom = std::clamp(m_targetZoom * std::pow(1.25f, wheelY), m_zoomMin, m_zoomMax);
}

void EditorCamera::BeginDrag(float mouseX, float mouseY)
{
    m_dragging = true;
    ScreenToWorld(mouseX, mouseY, m_dragWorldX, m_dragWorldY);
}

void EditorCamera::Update(float dt, float mouseX, float mouseY, int viewportW, int viewportH)
{
    m_viewportW = std::max(1, viewportW);
    m_viewportH = std::max(1, viewportH);

    // Keyboard pan moves at constant *screen* speed regardless of zoom.
    if (m_panX != 0.0f || m_panY != 0.0f) {
        const float length = std::sqrt(m_panX * m_panX + m_panY * m_panY);
        m_x += (m_panX / length) * m_panSpeed / m_zoom * dt;
        m_y += (m_panY / length) * m_panSpeed / m_zoom * dt;
    }

    // Zoom is centred on the screen: the world point at the viewport centre is
    // (m_x, m_y) whatever the zoom, so easing m_zoom alone keeps it fixed and
    // no camera translation is needed.
    if (std::fabs(m_targetZoom - m_zoom) > 1e-4f * m_zoom) {
        const float rate = 1.0f - std::exp(-14.0f * dt);
        m_zoom += (m_targetZoom - m_zoom) * rate;
        m_zoom = std::clamp(m_zoom, m_zoomMin, m_zoomMax);
    } else {
        m_zoom = m_targetZoom;
    }

    // Middle-mouse drag: keep the grabbed world point pinned under the cursor.
    if (m_dragging) {
        float underX = 0.0f, underY = 0.0f;
        ScreenToWorld(mouseX, mouseY, underX, underY);
        m_x += m_dragWorldX - underX;
        m_y += m_dragWorldY - underY;
    }
}

} // namespace engine
