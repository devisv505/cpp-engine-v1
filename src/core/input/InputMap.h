#pragma once

#include <array>
#include <string>
#include <vector>

#include <SDL3/SDL_scancode.h>

#include "core/input/Action.h"

namespace engine {

    class Input;

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

            [[nodiscard]] bool IsDown(const Input& input, Action action) const;
            [[nodiscard]] bool WasPressed(const Input& input, Action action) const;

        private:
            void Bind(Action action, std::initializer_list<SDL_Scancode> scancodes);

            // Several keys may drive one action (W and Up both pan up).
            std::array<std::vector<SDL_Scancode>, kActionCount> m_bindings;
    };

} // namespace engine
