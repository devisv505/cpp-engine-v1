#pragma once

#include <string>

#include "core/input/InputMap.h"
#include "core/input/InputState.h"

namespace engine {

    // Front door of the input system: owns the device-state snapshot
    // (InputState) and the action bindings (InputMap) and wires them together,
    // so consumers can ask directly whether an action is active. Pure
    // delegation -- state logic lives in InputState, binding semantics in
    // InputMap; anything more belongs in one of those.
    class Input {
        friend class InputFrame;
        public:
            // Subscribes the state to the engine input events (call once,
            // before the first pump) and loads the action bindings, falling
            // back to the defaults if the file is missing or invalid.
            void Init(EventBus& events, const std::string& bindingsPath)
            {
                m_state.Init(events);
                m_map.Load(bindingsPath);
            }

            [[nodiscard]] bool IsActionDown(const Action action) const
            {
                return m_map.IsDown(m_state, action);
            }

            [[nodiscard]] bool WasActionPressed(const Action action) const
            {
                return m_map.WasPressed(m_state, action);
            }

            // Raw device state, for input that actions do not cover (mouse
            // position, wheel delta, button edges).
            [[nodiscard]] const InputState& State() const { return m_state; }

            // Drops all held keys and buttons (e.g. on world rebuild).
            void Clear() { m_state.Clear(); }

        private:
            InputState m_state;
            InputMap   m_map;
    };

    // Declare one at the top of each iteration of the main loop:
    //
    //     while (running) { InputFrame frame(m_input); ... }
    //
    // The destructor advances the input state, so edge queries stay correct
    // even if the loop gains an early continue or break -- the case a
    // hand-written EndFrame() call silently gets wrong. The InputState
    // overload serves tests and anything else holding bare state.
    class InputFrame {
        public:
            explicit InputFrame(Input& input) : m_state(input.m_state) {}
            explicit InputFrame(InputState& state) : m_state(state) {}
            ~InputFrame() { m_state.EndFrame(); }

            InputFrame(const InputFrame&) = delete;
            InputFrame& operator=(const InputFrame&) = delete;

        private:
            InputState& m_state;
    };

} // namespace engine
