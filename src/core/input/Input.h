#pragma once
#include <SDL3/SDL_events.h>
#include <glm/vec2.hpp>

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

class Input {
    public:
        void ProcessEvent(const SDL_Event& event);

        [[nodiscard]] bool IsKeyPressed(Key key) const;
        [[nodiscard]] bool IsMouseButtonPressed(MouseButton button) const;

        [[nodiscard]] glm::vec2 GetMousePosition() const;
};
