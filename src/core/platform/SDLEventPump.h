#pragma once

namespace engine {

    class EventBus;

    // The only component that polls and understands raw SDL events. Pump()
    // drains SDL's queue once per frame and republishes what the engine cares
    // about as the engine-defined events in Events.h; everything else
    // (Window, InputState, renderers, Application) subscribes to those and never
    // sees an SDL_Event.
    class SDLEventPump {
        public:
            explicit SDLEventPump(EventBus& events) : m_events(events) {}

            SDLEventPump(const SDLEventPump&) = delete;
            SDLEventPump& operator=(const SDLEventPump&) = delete;

            // Call once per frame, before anything reads input state.
            void Pump();

        private:
            EventBus& m_events;
    };

} // namespace engine
