#include "core/platform/SDLEventPump.h"

#include <SDL3/SDL_events.h>

#include "core/events/EventBus.h"
#include "core/events/Events.h"

namespace engine {

    namespace {

        // Engine scancodes share SDL's values (both are USB HID); the range
        // check is the whole translation.
        bool ToScancode(const SDL_Scancode sdl, Scancode& out)
        {
            if (sdl <= SDL_SCANCODE_UNKNOWN || sdl >= SDL_SCANCODE_COUNT) {
                return false;
            }
            out = static_cast<Scancode>(sdl);
            return true;
        }

        bool ToMouseButton(const Uint8 sdlButton, MouseButton& out)
        {
            switch (sdlButton) {
            case SDL_BUTTON_LEFT:   out = MouseButton::Left;   return true;
            case SDL_BUTTON_MIDDLE: out = MouseButton::Middle; return true;
            case SDL_BUTTON_RIGHT:  out = MouseButton::Right;  return true;
            case SDL_BUTTON_X1:     out = MouseButton::X1;     return true;
            case SDL_BUTTON_X2:     out = MouseButton::X2;     return true;
            default:                return false;
            }
        }

    } // namespace

    void SDLEventPump::Pump()
    {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                m_events.Emit(QuitRequested{});
                break;

            // Single-window engine: any close request means quit, so no
            // window-id filtering is needed.
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                m_events.Emit(QuitRequested{});
                break;

            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                m_events.Emit(WindowResized{{event.window.data1, event.window.data2}});
                break;

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                m_events.Emit(WindowFocusLost{});
                break;

            case SDL_EVENT_KEY_DOWN: {
                // Auto-repeat re-sends KEY_DOWN while a key is held; KeyPressed
                // means the actual press, so repeats are dropped here.
                Scancode scancode;
                if (!event.key.repeat && ToScancode(event.key.scancode, scancode)) {
                    m_events.Emit(KeyPressed{scancode});
                }
                break;
            }

            case SDL_EVENT_KEY_UP: {
                Scancode scancode;
                if (ToScancode(event.key.scancode, scancode)) {
                    m_events.Emit(KeyReleased{scancode});
                }
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                MouseButton button;
                if (ToMouseButton(event.button.button, button)) {
                    const Vector2 position{event.button.x, event.button.y};
                    if (event.button.down) {
                        m_events.Emit(MouseButtonPressed{button, position});
                    } else {
                        m_events.Emit(MouseButtonReleased{button, position});
                    }
                }
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                m_events.Emit(MouseMoved{{event.motion.x, event.motion.y}});
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                m_events.Emit(MouseWheel{event.wheel.y,
                                         {event.wheel.mouse_x, event.wheel.mouse_y}});
                break;

            default:
                break;
            }
        }
    }

} // namespace engine
