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

    bool Input::IsKeyPressed(const Key key) const
    {
        const SDL_Scancode scancode = ToSDLScancode(key);
        if (scancode == SDL_SCANCODE_UNKNOWN) {
            return false;
        }
        return m_keys[static_cast<std::size_t>(scancode)];
    }

    bool Input::IsMouseButtonPressed(MouseButton button) const
    {
        const auto index = static_cast<std::size_t>(button);
        if (index >= m_mouseButtons.size()) {
            return false;
        }
        return m_mouseButtons[index];
    }

    Vector2 Input::GetMousePosition() const
    {
        return m_mousePosition;
    }

    void Input::Clear()
    {
        m_keys.fill(false);
        m_mouseButtons.fill(false);
    }

    SDL_Scancode Input::ToSDLScancode(Key key)
    {
        switch (key) {
        case Key::W: return SDL_SCANCODE_W;
        case Key::A: return SDL_SCANCODE_A;
        case Key::S: return SDL_SCANCODE_S;
        case Key::D: return SDL_SCANCODE_D;

        case Key::Up:    return SDL_SCANCODE_UP;
        case Key::Down:  return SDL_SCANCODE_DOWN;
        case Key::Left:  return SDL_SCANCODE_LEFT;
        case Key::Right: return SDL_SCANCODE_RIGHT;

        case Key::Space:     return SDL_SCANCODE_SPACE;
        case Key::Enter:     return SDL_SCANCODE_RETURN;
        case Key::Escape:    return SDL_SCANCODE_ESCAPE;
        case Key::Tab:       return SDL_SCANCODE_TAB;
        case Key::Backspace: return SDL_SCANCODE_BACKSPACE;

        case Key::LeftShift:  return SDL_SCANCODE_LSHIFT;
        case Key::RightShift: return SDL_SCANCODE_RSHIFT;
        case Key::LeftCtrl:   return SDL_SCANCODE_LCTRL;
        case Key::RightCtrl:  return SDL_SCANCODE_RCTRL;
        case Key::LeftAlt:    return SDL_SCANCODE_LALT;
        case Key::RightAlt:   return SDL_SCANCODE_RALT;

        case Key::F1:  return SDL_SCANCODE_F1;
        case Key::F2:  return SDL_SCANCODE_F2;
        case Key::F3:  return SDL_SCANCODE_F3;
        case Key::F4:  return SDL_SCANCODE_F4;
        case Key::F5:  return SDL_SCANCODE_F5;
        case Key::F6:  return SDL_SCANCODE_F6;
        case Key::F7:  return SDL_SCANCODE_F7;
        case Key::F8:  return SDL_SCANCODE_F8;
        case Key::F9:  return SDL_SCANCODE_F9;
        case Key::F10: return SDL_SCANCODE_F10;
        case Key::F11: return SDL_SCANCODE_F11;
        case Key::F12: return SDL_SCANCODE_F12;

        case Key::Unknown: break;
        }
        // No default: adding a Key without a mapping is then a compiler warning
        // rather than a silently dead binding.
        return SDL_SCANCODE_UNKNOWN;
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
