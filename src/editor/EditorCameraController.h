#pragma once

namespace engine {

    class EditorCamera;
    class Input;
    class Window;

    // Drives EditorCamera from input state each frame: action-bound panning,
    // wheel zoom, middle-mouse drag. EditorCamera itself stays pure camera
    // state and math; this class is the only place that knows which inputs
    // mean what to the editor view.
    class EditorCameraController {
        public:
            EditorCameraController(EditorCamera& camera, const Input& input,
                                   const Window& window)
                : m_camera(camera), m_input(input), m_window(window) {}

            void Update(float deltaTime) const;

        private:
            EditorCamera& m_camera;
            const Input&  m_input;
            const Window& m_window;
    };

} // namespace engine
