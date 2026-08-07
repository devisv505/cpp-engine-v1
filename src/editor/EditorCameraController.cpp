#include "editor/EditorCameraController.h"

#include "core/input/Input.h"
#include "core/input/InputMap.h"
#include "core/Window.h"
#include "editor/EditorCamera.h"

namespace engine {

    void EditorCameraController::Update(const float deltaTime) const {
        int pixelW = 0, pixelH = 0;
        m_window.GetPixelSize(pixelW, pixelH);

        // Input reports window points; the camera works in framebuffer pixels.
        const auto [x, y] = m_input.GetMousePosition() * m_window.GetPixelDensity();

        Vector2 direction{};
        if (m_inputMap.IsDown(m_input, Action::CameraUp))    direction.y -= 1.0f;
        if (m_inputMap.IsDown(m_input, Action::CameraDown))  direction.y += 1.0f;
        if (m_inputMap.IsDown(m_input, Action::CameraLeft))  direction.x -= 1.0f;
        if (m_inputMap.IsDown(m_input, Action::CameraRight)) direction.x += 1.0f;
        m_camera.SetPanInput(direction.x, direction.y);

        if (const float wheel = m_input.GetWheelDelta(); wheel != 0.0f) {
            m_camera.OnMouseWheel(wheel);
        }

        // Edges, not held state: re-anchoring the drag every frame would pin
        // the map to the cursor and it would never move.
        if (m_input.WasMouseButtonPressed(MouseButton::Middle)) {
            m_camera.BeginDrag(x, y);
        }
        if (m_input.WasMouseButtonReleased(MouseButton::Middle)) {
            m_camera.EndDrag();
        }

        m_camera.Update(deltaTime, x, y, pixelW, pixelH);
    }

} // namespace engine
