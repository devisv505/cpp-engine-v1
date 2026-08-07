#pragma once

#include <cstddef>
#include <cstdint>

namespace engine {

    // Physical key positions, valued identically to SDL scancodes (both follow
    // the USB HID usage table). SDLEventPump relies on that identity to
    // translate with a plain cast, and InputMap resolves configured key names
    // through SDL the same way — so any SDL scancode is representable here,
    // named below or not.
    //
    // Only keys the engine names in code are enumerated; add more as needed.
    enum class Scancode : std::uint16_t {
        Unknown = 0,

        A = 4, B, C, D, E, F, G, H, I, J, K, L, M,
        N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

        Digit1 = 30, Digit2, Digit3, Digit4, Digit5,
        Digit6, Digit7, Digit8, Digit9, Digit0,

        Return    = 40,
        Escape    = 41,
        Backspace = 42,
        Tab       = 43,
        Space     = 44,

        F1 = 58, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

        Right = 79,
        Left  = 80,
        Down  = 81,
        Up    = 82,

        Keypad1 = 89, Keypad2, Keypad3, Keypad4, Keypad5,
        Keypad6, Keypad7, Keypad8, Keypad9, Keypad0,

        LeftControl  = 224,
        LeftShift    = 225,
        LeftAlt      = 226,
        RightControl = 228,
        RightShift   = 229,
        RightAlt     = 230,
    };

    // Size of the scancode space (mirrors SDL_SCANCODE_COUNT); state arrays
    // index by scancode value.
    inline constexpr std::size_t kScancodeCount = 512;

    enum class MouseButton {
        Left,
        Middle,
        Right,
        X1,
        X2,

        Count
    };

} // namespace engine
