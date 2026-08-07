#pragma once

#include "core/input/InputCodes.h"
#include "core/math/Vector2.h"
#include "core/math/Vector2Int.h"

namespace engine {

    // Engine-wide event types. Each is a plain struct: the bus dispatches on
    // the type itself, so no ids, tags or base class are needed.
    //
    // Window and input events are published by SDLEventPump, the only
    // component that reads raw SDL events. Mouse positions are in window
    // points, not framebuffer pixels — scale by the window's pixel density
    // if you need pixels.

    // The user asked to close the application: OS quit, or the window's close
    // button.
    struct QuitRequested {
    };

    // The framebuffer changed size (window resized, or moved to a display with
    // a different pixel density). Carries pixels, not window points.
    struct WindowResized {
        Vector2Int size;
    };

    // The window lost input focus. Key-up events go to whoever has focus now,
    // so held-key state must not be trusted past this point.
    struct WindowFocusLost {
    };

    // A key went down. Auto-repeat does not re-fire this.
    struct KeyPressed {
        Scancode scancode;
    };

    struct KeyReleased {
        Scancode scancode;
    };

    // Button events carry the cursor position so a click is located even
    // without a preceding MouseMoved.
    struct MouseButtonPressed {
        MouseButton button;
        Vector2     position;
    };

    struct MouseButtonReleased {
        MouseButton button;
        Vector2     position;
    };

    struct MouseMoved {
        Vector2 position;
    };

    // One notch of wheel travel; positive is up. Several can land in a frame.
    struct MouseWheel {
        float   delta;
        Vector2 position;
    };

    // The Lua script was re-run and the world rebuilt.
    struct WorldRebuilt {
        Vector2Int mapSize;  // in tiles
    };

} // namespace engine
