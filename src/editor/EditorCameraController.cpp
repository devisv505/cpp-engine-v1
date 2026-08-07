#include "editor/EditorCameraController.h"

#include "core/input/Input.h"
#include "core/Window.h"
#include "editor/EditorCamera.h"

namespace engine {

    void EditorCameraController::Update(const float deltaTime) const {
        int pixelW = 0, pixelH = 0;
        m_window.GetPixelSize(pixelW, pixelH);

        const InputState& state = m_input.State();

        // Input reports window points; the camera works in framebuffer pixels.
        const auto [x, y] = state.GetMousePosition() * m_window.GetPixelDensity();

        Vector2 direction{};
        if (m_input.IsActionDown(Action::CameraUp))    direction.y -= 1.0f;
        if (m_input.IsActionDown(Action::CameraDown))  direction.y += 1.0f;
        if (m_input.IsActionDown(Action::CameraLeft))  direction.x -= 1.0f;
        if (m_input.IsActionDown(Action::CameraRight)) direction.x += 1.0f;
        m_camera.SetPanInput(direction.x, direction.y);

        if (const float wheel = state.GetWheelDelta(); wheel != 0.0f) {
            m_camera.OnMouseWheel(wheel);
        }

        // Edges, not held state: re-anchoring the drag every frame would pin
        // the map to the cursor and it would never move.
        if (state.WasMouseButtonPressed(MouseButton::Middle)) {
            m_camera.BeginDrag(x, y);
        }
        if (state.WasMouseButtonReleased(MouseButton::Middle)) {
            m_camera.EndDrag();
        }

        m_camera.Update(deltaTime, x, y, pixelW, pixelH);
    }

} // namespace engine
