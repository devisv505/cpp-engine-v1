#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/input/Action.h"
#include "core/input/InputCodes.h"

namespace engine {

    class InputState;

    // Binds physical keys to actions, loaded from config/input.json.
    //
    // Scancodes rather than keycodes: bindings name physical key positions, so
    // the WASD cluster stays under the same fingers on layouts where those keys
    // are labelled differently.
    class InputMap {
        public:
            InputMap();  // starts with the built-in defaults

            // Replaces the bindings with the file's. A missing or invalid file
            // logs and leaves the defaults in place, so controls always work.
            bool Load(const std::string& path);

            void SetDefaults();

            [[nodiscard]] bool IsDown(const InputState& input, Action action) const;
            [[nodiscard]] bool WasPressed(const InputState& input, Action action) const;

        private:
            void Bind(Action action, std::initializer_list<Scancode> scancodes);

            // Several keys may drive one action (W and Up both pan up).
            std::array<std::vector<Scancode>, kActionCount> m_bindings;
    };

} // namespace engine
