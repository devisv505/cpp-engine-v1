#include "TestSupport.h"

#include <fstream>

#include "core/events/EventBus.h"
#include "core/events/Events.h"
#include "core/input/Input.h"
#include "core/input/InputMap.h"
#include "core/input/InputState.h"

using namespace engine;
using tests::TestRun;

namespace {

    // InputState is fed through the bus, exactly as SDLEventPump feeds it at
    // runtime. Each fixture is its own bus + subscribed InputState, so multiple
    // fixtures in one test never hear each other's events.
    struct BusInput {
        EventBus bus;
        InputState input;

        BusInput() { input.Init(bus); }

        void Press(const Scancode key)      { bus.Emit(KeyPressed{key}); }
        void Release(const Scancode key)    { bus.Emit(KeyReleased{key}); }
        void Press(const MouseButton button, const float x = 0.0f, const float y = 0.0f)
        {
            bus.Emit(MouseButtonPressed{button, {x, y}});
        }
        void Release(const MouseButton button, const float x = 0.0f, const float y = 0.0f)
        {
            bus.Emit(MouseButtonReleased{button, {x, y}});
        }
    };

} // namespace

int main()
{
    TestRun t("input");

    {
        BusInput fixture;
        InputState& input = fixture.input;
        CHECK(t, "nothing is down initially", !input.IsScancodeDown(Scancode::W));

        fixture.Press(Scancode::W);
        CHECK(t, "key reads as down while held", input.IsScancodeDown(Scancode::W));
        fixture.Press(Scancode::W);  // duplicate press event
        CHECK(t, "a duplicate press keeps it down", input.IsScancodeDown(Scancode::W));
        fixture.Release(Scancode::W);
        CHECK(t, "key clears on release", !input.IsScancodeDown(Scancode::W));

        fixture.Press(MouseButton::X2, 10.0f, 20.0f);
        CHECK(t, "X2 maps to the right button", input.IsMouseButtonDown(MouseButton::X2));
        CHECK(t, "click position is tracked without prior motion",
              input.GetMousePosition().x == 10.0f && input.GetMousePosition().y == 20.0f);
        fixture.bus.Emit(MouseButtonPressed{static_cast<MouseButton>(99), {}});
        CHECK(t, "an out-of-range button is ignored safely",
              !input.IsMouseButtonDown(MouseButton::Left));

        // A key held while focus is lost would never receive its key-up.
        fixture.Press(Scancode::LeftShift);
        fixture.bus.Emit(WindowFocusLost{});
        CHECK(t, "focus loss releases everything",
              !input.IsScancodeDown(Scancode::LeftShift)
              && !input.IsMouseButtonDown(MouseButton::X2));
    }

    {   // Edges: the reason one-shot actions can be polled instead of evented.
        BusInput fixture;
        InputState& input = fixture.input;
        fixture.Press(Scancode::F5);
        CHECK(t, "press edge fires once", input.WasScancodePressed(Scancode::F5));
        { const InputFrame frame(input); }
        CHECK(t, "edge does not repeat while held",
              !input.WasScancodePressed(Scancode::F5) && input.IsScancodeDown(Scancode::F5));

        fixture.Press(MouseButton::Middle);
        CHECK(t, "mouse press edge", input.WasMouseButtonPressed(MouseButton::Middle));
        { const InputFrame frame(input); }
        fixture.Release(MouseButton::Middle);
        CHECK(t, "mouse release edge", input.WasMouseButtonReleased(MouseButton::Middle));

        fixture.bus.Emit(MouseWheel{1.0f, {}});
        fixture.bus.Emit(MouseWheel{2.0f, {}});
        CHECK(t, "wheel ticks accumulate within a frame", input.GetWheelDelta() == 3.0f);
        { const InputFrame frame(input); }
        CHECK(t, "wheel clears at frame end", input.GetWheelDelta() == 0.0f);
    }

    {   // The RAII guard must hold even when the loop body exits early.
        BusInput fixture;
        InputState& input = fixture.input;
        InputMap map;
        int      fires = 0;
        fixture.Press(Scancode::F5);  // held throughout
        for (int frame = 0; frame < 6; ++frame) {
            const InputFrame guard(input);
            if (map.WasPressed(input, Action::ReloadScript)) {
                ++fires;
            }
            if (frame % 2 == 0) {
                continue;  // the case a manual EndFrame() at the bottom misses
            }
        }
        CHECK(t, "held action fires once across frames, even with continue", fires == 1);

        fixture.Press(Scancode::Escape);
        try {
            const InputFrame guard(input);
            throw 1;
        } catch (int) {
        }
        CHECK(t, "guard advances the frame even when the scope unwinds",
              !map.WasPressed(input, Action::Quit));
    }

    {   // Action bindings, defaults and JSON loading.
        InputMap map;
        BusInput w;
        w.Press(Scancode::W);
        CHECK(t, "default binding: W drives camera_up", map.IsDown(w.input, Action::CameraUp));
        BusInput arrows;
        arrows.Press(Scancode::Up);
        CHECK(t, "a second key drives the same action", map.IsDown(arrows.input, Action::CameraUp));

        const char* path = "input_tests_bindings.json";
        {
            std::ofstream file(path);
            file << R"({"actions":{"camera_up":["Keypad 8"],"camera_left":"Q",)"
                    R"("camera_right":["NotAKey"],"bogus_action":["X"]}})";
        }
        InputMap custom;
        CHECK(t, "Load succeeds", custom.Load(path));
        BusInput keypad;
        keypad.Press(Scancode::Keypad8);
        CHECK(t, "rebound key takes effect", custom.IsDown(keypad.input, Action::CameraUp));
        CHECK(t, "the old default stops working", !custom.IsDown(w.input, Action::CameraUp));
        BusInput q;
        q.Press(Scancode::Q);
        CHECK(t, "a bare string binds too", custom.IsDown(q.input, Action::CameraLeft));
        BusInput d;
        d.Press(Scancode::D);
        CHECK(t, "an unresolvable key name keeps the default",
              custom.IsDown(d.input, Action::CameraRight));
        BusInput escape;
        escape.Press(Scancode::Escape);
        CHECK(t, "an omitted action keeps its default", custom.IsDown(escape.input, Action::Quit));
        std::remove(path);

        InputMap missing;
        CHECK(t, "a missing file reports failure", !missing.Load("input_tests_absent.json"));
        CHECK(t, "...and leaves the controls working", missing.IsDown(w.input, Action::CameraUp));
    }

    {   // The Input facade: state + bindings behind one front door.
        EventBus bus;
        Input    input;
        input.Init(bus, "input_tests_absent.json");  // missing file -> defaults
        bus.Emit(KeyPressed{Scancode::W});
        CHECK(t, "facade answers action queries from bus-fed state",
              input.IsActionDown(Action::CameraUp));
        CHECK(t, "press edge shows through the facade",
              input.WasActionPressed(Action::CameraUp));
        { const InputFrame frame(input); }
        CHECK(t, "InputFrame advances the facade's state",
              input.IsActionDown(Action::CameraUp)
              && !input.WasActionPressed(Action::CameraUp));
        CHECK(t, "raw device state is reachable behind State()",
              input.State().IsScancodeDown(Scancode::W));
        input.Clear();
        CHECK(t, "Clear drops held state", !input.IsActionDown(Action::CameraUp));
    }

    return t.Result();
}
