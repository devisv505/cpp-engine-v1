#pragma once
#include <array>
#include <SDL3/SDL_events.h>
#include "core/math/Vector2.h"

namespace engine {

    enum class Key
    {
        Unknown,

        W,
        A,
        S,
        D,

        Up,
        Down,
        Left,
        Right,

        Space,
        Enter,
        Escape,
        Tab,
        Backspace,

        LeftShift,
        RightShift,
        LeftCtrl,
        RightCtrl,
        LeftAlt,
        RightAlt,

        F1,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12
    };

    enum class MouseButton
    {
        Left,
        Middle,
        Right,
        X1,
        X2,

        Count
    };

    // Keyboard and mouse state, fed by the application's event loop.
    //
    // Reports what is held *right now*: IsKeyPressed is true for as long as the
    // key is down, not just on the frame it went down.
    class Input {
        public:
            void ProcessEvent(const SDL_Event& event);

            [[nodiscard]] bool IsKeyPressed(Key key) const;
            [[nodiscard]] bool IsMouseButtonPressed(MouseButton button) const;

            // Window coordinates, not framebuffer pixels: SDL reports events in
            // points, and this class has no window to ask about pixel density.
            // Scale by the window's density if you need pixels.
            [[nodiscard]] engine::Vector2 GetMousePosition() const;

            // Drops all held keys and buttons. Called on focus loss so keys do
            // not stay stuck down while the app is in the background.
            void Clear();

        private:
            static SDL_Scancode ToSDLScancode(Key key);
            static int ToMouseButtonIndex(Uint8 sdlButton);

            std::array<bool, SDL_SCANCODE_COUNT> m_keys{};

            std::array<
                bool,
                static_cast<std::size_t>(MouseButton::Count)
            > m_mouseButtons{};

            Vector2 m_mousePosition{};
    };

} // namespace engine
