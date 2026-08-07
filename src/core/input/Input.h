#pragma once
#include <array>
#include <vector>

#include "core/events/EventBus.h"
#include "core/input/InputCodes.h"
#include "core/math/Vector2.h"

namespace engine {

    // Raw keyboard and mouse state, fed by the input events SDLEventPump
    // publishes on the EventBus. Knows nothing about what any key means --
    // see InputMap for actions.
    //
    // "Down" queries report what is held right now; "Was" queries report the
    // transition that happened during this frame. Advancing to the next frame
    // is the job of InputFrame below, which cannot be forgotten.
    class Input {
        friend class InputFrame;
        public:
            // Subscribes to the engine input events. Call once, before the
            // first pump; the subscriptions last as long as this Input.
            void Init(EventBus& events);

            // Keys are identified by physical scancode. Nothing here knows
            // what a key means -- InputMap turns scancodes into actions.
            [[nodiscard]] bool IsScancodeDown(Scancode scancode) const;
            [[nodiscard]] bool WasScancodePressed(Scancode scancode) const;

            [[nodiscard]] bool IsMouseButtonDown(MouseButton button) const;
            [[nodiscard]] bool WasMouseButtonPressed(MouseButton button) const;
            [[nodiscard]] bool WasMouseButtonReleased(MouseButton button) const;

            // Wheel ticks accumulated since the last EndFrame; positive is up.
            [[nodiscard]] float GetWheelDelta() const;

            // Window coordinates, not framebuffer pixels: input events report
            // points, and this class has no window to ask about pixel density.
            // Scale by the window's density if you need pixels.
            [[nodiscard]] engine::Vector2 GetMousePosition() const;

            // Drops all held keys and buttons. Runs on focus loss so keys do
            // not stay stuck down while the app is in the background.
            void Clear();

        private:
            // Rolls current state into previous and clears the wheel delta.
            // Private so it can only happen via InputFrame.
            void EndFrame();

            void SetKey(Scancode scancode, bool down);
            void SetMouseButton(MouseButton button, bool down);

            std::array<bool, kScancodeCount> m_keys{};
            std::array<bool, kScancodeCount> m_previousKeys{};

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

            std::vector<Subscription> m_subscriptions;
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
