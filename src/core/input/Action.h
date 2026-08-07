#pragma once

#include <cstddef>

namespace engine {

    // What the engine can be asked to do, independent of which key does it.
    // Bindings live in config/input.json; adding an action is a C++ change,
    // which keeps every query compile-checked.
    enum class Action
    {
        CameraUp,
        CameraDown,
        CameraLeft,
        CameraRight,

        ReloadScript,
        Quit,

        Count
    };

    // The name used for this action in config/input.json.
    [[nodiscard]] constexpr const char* ToString(const Action action)
    {
        switch (action) {
        case Action::CameraUp:     return "camera_up";
        case Action::CameraDown:   return "camera_down";
        case Action::CameraLeft:   return "camera_left";
        case Action::CameraRight:  return "camera_right";
        case Action::ReloadScript: return "reload_script";
        case Action::Quit:         return "quit";
        case Action::Count:        break;
        }
        // No default: a new Action without a name is a compiler warning rather
        // than a binding that silently never loads.
        return "";
    }

    inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);

} // namespace engine
