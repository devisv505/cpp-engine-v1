#pragma once
#include <array>
#include <SDL3/SDL_events.h>
#include "core/math/Vector2.h"

namespace engine {

    enum class MouseButton
    {
        Left,
        Middle,
        Right,
        X1,
        X2,

        Count
    };

    // Raw keyboard and mouse state, fed by the application's event loop. Knows
    // nothing about what any key means -- see InputMap for actions.
    //
    // "Down" queries report what is held right now; "Was" queries report the
    // transition that happened during this frame. Advancing to the next frame
    // is the job of InputFrame below, which cannot be forgotten.
    class Input {
        friend class InputFrame;
        public:
            void ProcessEvent(const SDL_Event& event);

            // Keys are identified by physical scancode. Nothing here knows
            // what a key means -- InputMap turns scancodes into actions.
            [[nodiscard]] bool IsScancodeDown(SDL_Scancode scancode) const;
            [[nodiscard]] bool WasScancodePressed(SDL_Scancode scancode) const;

            [[nodiscard]] bool IsMouseButtonDown(MouseButton button) const;
            [[nodiscard]] bool WasMouseButtonPressed(MouseButton button) const;
            [[nodiscard]] bool WasMouseButtonReleased(MouseButton button) const;

            // Wheel ticks accumulated since the last EndFrame; positive is up.
            [[nodiscard]] float GetWheelDelta() const;

            // Window coordinates, not framebuffer pixels: SDL reports events in
            // points, and this class has no window to ask about pixel density.
            // Scale by the window's density if you need pixels.
            [[nodiscard]] engine::Vector2 GetMousePosition() const;

            // Drops all held keys and buttons. Called on focus loss so keys do
            // not stay stuck down while the app is in the background.
            void Clear();

        private:
            // Rolls current state into previous and clears the wheel delta.
            // Private so it can only happen via InputFrame.
            void EndFrame();

            static int ToMouseButtonIndex(Uint8 sdlButton);

            std::array<bool, SDL_SCANCODE_COUNT> m_keys{};
            std::array<bool, SDL_SCANCODE_COUNT> m_previousKeys{};

            std::array<
                bool,
                static_cast<std::size_t>(MouseButton::Count)
            > m_mouseButtons{};
            std::array<
                bool,
                static_cast<std::size_t>(MouseButton::Count)
            > m_previousMouseButtons{};

            Vector2 m_mousePosition{};
            float   m_wheelDelta = 0.0f;
    };

    // Declare one at the top of each iteration of the main loop:
    //
    //     while (running) { InputFrame frame(m_input); ... }
    //
    // The destructor advances the input, so edge queries stay correct even if
    // the loop gains an early continue or break -- the case a hand-written
    // EndFrame() call silently gets wrong.
    class InputFrame {
        public:
            explicit InputFrame(Input& input) : m_input(input) {}
            ~InputFrame() { m_input.EndFrame(); }

            InputFrame(const InputFrame&) = delete;
            InputFrame& operator=(const InputFrame&) = delete;

        private:
            Input& m_input;
    };

} // namespace engine
