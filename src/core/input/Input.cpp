#include "core/input/Input.h"

#include "core/events/Events.h"

namespace engine {

    void Input::Init(EventBus& events)
    {
        m_subscriptions.clear();

        m_subscriptions.push_back(events.Subscribe<KeyPressed>(
            [this](const KeyPressed& e) { SetKey(e.scancode, true); }));

        m_subscriptions.push_back(events.Subscribe<KeyReleased>(
            [this](const KeyReleased& e) { SetKey(e.scancode, false); }));

        m_subscriptions.push_back(events.Subscribe<MouseButtonPressed>(
            [this](const MouseButtonPressed& e) {
                SetMouseButton(e.button, true);
                m_mousePosition = e.position;
            }));

        m_subscriptions.push_back(events.Subscribe<MouseButtonReleased>(
            [this](const MouseButtonReleased& e) {
                SetMouseButton(e.button, false);
                m_mousePosition = e.position;
            }));

        m_subscriptions.push_back(events.Subscribe<MouseMoved>(
            [this](const MouseMoved& e) { m_mousePosition = e.position; }));

        m_subscriptions.push_back(events.Subscribe<MouseWheel>(
            [this](const MouseWheel& e) {
                // Ticks accumulate: several wheel events can land in one frame.
                m_wheelDelta   += e.delta;
                m_mousePosition = e.position;
            }));

        m_subscriptions.push_back(events.Subscribe<WindowFocusLost>(
            [this](const WindowFocusLost&) {
                // Key-up arrives at whoever has focus, so without this a key
                // held while alt-tabbing away would stay down forever.
                Clear();
            }));
    }

    void Input::SetKey(const Scancode scancode, const bool down)
    {
        const auto index = static_cast<std::size_t>(scancode);
        if (index > 0 && index < kScancodeCount) {
            m_keys[index] = down;
        }
    }

    void Input::SetMouseButton(const MouseButton button, const bool down)
    {
        const auto index = static_cast<std::size_t>(button);
        if (index < m_mouseButtons.size()) {
            m_mouseButtons[index] = down;
        }
    }

    bool Input::IsScancodeDown(const Scancode scancode) const
    {
        const auto index = static_cast<std::size_t>(scancode);
        if (index == 0 || index >= kScancodeCount) {
            return false;
        }
        return m_keys[index];
    }

    bool Input::WasScancodePressed(const Scancode scancode) const
    {
        const auto index = static_cast<std::size_t>(scancode);
        if (index == 0 || index >= kScancodeCount) {
            return false;
        }
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

} // namespace engine
