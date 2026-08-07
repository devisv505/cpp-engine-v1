#include "core/input/Input.h"

namespace engine {

    void Input::ProcessEvent(const SDL_Event& event)
    {
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            // Auto-repeat re-sends KEY_DOWN for a key already held; the state is
            // the same either way, so repeats need no special handling.
            const SDL_Scancode scancode = event.key.scancode;
            if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT) {
                m_keys[static_cast<std::size_t>(scancode)] = event.key.down;
            }
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const int index = ToMouseButtonIndex(event.button.button);
            if (index >= 0) {
                m_mouseButtons[static_cast<std::size_t>(index)] = event.button.down;
            }
            // Button events carry a position too, so tracking it here keeps the
            // cursor correct even for a click with no preceding motion event.
            m_mousePosition = {event.button.x, event.button.y};
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL:
            // Ticks accumulate: several wheel events can land in one frame.
            m_wheelDelta += event.wheel.y;
            m_mousePosition = {event.wheel.mouse_x, event.wheel.mouse_y};
            break;

        case SDL_EVENT_MOUSE_MOTION:
            m_mousePosition = {event.motion.x, event.motion.y};
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // Key-up arrives at whoever has focus, so without this a key held
            // while alt-tabbing away would stay down forever.
            Clear();
            break;

        default:
            break;
        }
    }

    bool Input::IsScancodeDown(const SDL_Scancode scancode) const
    {
        if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
            return false;
        }
        return m_keys[static_cast<std::size_t>(scancode)];
    }

    bool Input::WasScancodePressed(const SDL_Scancode scancode) const
    {
        if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
            return false;
        }
        const auto index = static_cast<std::size_t>(scancode);
        return m_keys[index] && !m_previousKeys[index];
    }

    bool Input::IsMouseButtonDown(MouseButton button) const
    {
        const auto index = static_cast<std::size_t>(button);
        if (index >= m_mouseButtons.size()) {
            return false;
        }
        return m_mouseButtons[index];
    }

    bool Input::WasMouseButtonPressed(MouseButton button) const
    {
        const auto index = static_cast<std::size_t>(button);
        if (index >= m_mouseButtons.size()) {
            return false;
        }
        return m_mouseButtons[index] && !m_previousMouseButtons[index];
    }

    bool Input::WasMouseButtonReleased(MouseButton button) const
    {
        const auto index = static_cast<std::size_t>(button);
        if (index >= m_mouseButtons.size()) {
            return false;
        }
        return !m_mouseButtons[index] && m_previousMouseButtons[index];
    }

    float Input::GetWheelDelta() const
    {
        return m_wheelDelta;
    }

    void Input::EndFrame()
    {
        m_previousKeys         = m_keys;
        m_previousMouseButtons = m_mouseButtons;
        m_wheelDelta           = 0.0f;
    }

    Vector2 Input::GetMousePosition() const
    {
        return m_mousePosition;
    }

    void Input::Clear()
    {
        m_keys.fill(false);
        m_mouseButtons.fill(false);
        // Previous state clears too: a key held before focus was lost must not
        // register as a release edge on the frame focus comes back.
        m_previousKeys.fill(false);
        m_previousMouseButtons.fill(false);
        m_wheelDelta = 0.0f;
    }

    int Input::ToMouseButtonIndex(Uint8 sdlButton)
    {
        switch (sdlButton) {
        case SDL_BUTTON_LEFT:   return static_cast<int>(MouseButton::Left);
        case SDL_BUTTON_MIDDLE: return static_cast<int>(MouseButton::Middle);
        case SDL_BUTTON_RIGHT:  return static_cast<int>(MouseButton::Right);
        case SDL_BUTTON_X1:     return static_cast<int>(MouseButton::X1);
        case SDL_BUTTON_X2:     return static_cast<int>(MouseButton::X2);
        default:                return -1;
        }
    }

} // namespace engine
