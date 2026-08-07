#include "core/input/InputMap.h"

#include <fstream>

#include <nlohmann/json.hpp>
#include <SDL3/SDL_keyboard.h>

#include "core/Log.h"
#include "core/input/Input.h"

namespace engine {

    InputMap::InputMap()
    {
        SetDefaults();
    }

    void InputMap::Bind(const Action action, std::initializer_list<Scancode> scancodes)
    {
        m_bindings[static_cast<std::size_t>(action)].assign(scancodes);
    }

    // Mirrors config/input.json. These are the fallback when that file is
    // missing or an action in it fails to resolve, so the engine is always
    // controllable; keep the two in step when changing either.
    void InputMap::SetDefaults()
    {
        Bind(Action::CameraUp,     {Scancode::W, Scancode::Up});
        Bind(Action::CameraDown,   {Scancode::S, Scancode::Down});
        Bind(Action::CameraLeft,   {Scancode::A, Scancode::Left});
        Bind(Action::CameraRight,  {Scancode::D, Scancode::Right});
        Bind(Action::ReloadScript, {Scancode::F5});
        Bind(Action::Quit,         {Scancode::Escape});
    }

    bool InputMap::Load(const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open()) {
            LOG_WARN("Input bindings '%s' not found, using defaults", path.c_str());
            return false;
        }

        const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded()) {
            LOG_ERROR("Input bindings '%s' are not valid JSON, using defaults", path.c_str());
            return false;
        }

        const auto actions = doc.value("actions", nlohmann::json::object());
        if (actions.empty()) {
            LOG_WARN("Input bindings '%s' define no actions, using defaults", path.c_str());
            return false;
        }

        // Only actions the file names are replaced; anything it omits keeps its
        // default, so a partial file cannot leave the engine uncontrollable.
        for (std::size_t i = 0; i < kActionCount; ++i) {
            const auto  action = static_cast<Action>(i);
            const auto* name   = ToString(action);

            const auto entry = actions.find(name);
            if (entry == actions.end()) {
                continue;
            }

            std::vector<std::string> keyNames;
            if (entry->is_string()) {
                keyNames.push_back(entry->get<std::string>());
            } else if (entry->is_array()) {
                for (const auto& item : *entry) {
                    if (item.is_string()) {
                        keyNames.push_back(item.get<std::string>());
                    } else {
                        LOG_WARN("Input bindings: '%s' contains a non-string key", name);
                    }
                }
            } else {
                LOG_WARN("Input bindings: '%s' must be a key name or an array of them", name);
                continue;
            }

            std::vector<Scancode> scancodes;
            for (const std::string& keyName : keyNames) {
                // Key names resolve through SDL, the only place that knows
                // them all; the cast is exact because engine scancode values
                // mirror SDL's (see InputCodes.h).
                const SDL_Scancode scancode = SDL_GetScancodeFromName(keyName.c_str());
                if (scancode == SDL_SCANCODE_UNKNOWN) {
                    LOG_WARN("Input bindings: unknown key '%s' for action '%s'",
                             keyName.c_str(), name);
                    continue;
                }
                scancodes.push_back(static_cast<Scancode>(scancode));
            }

            if (scancodes.empty()) {
                LOG_WARN("Input bindings: '%s' resolved to no usable key, keeping the default",
                         name);
                continue;
            }
            m_bindings[i] = std::move(scancodes);
        }

        // Warn about names in the file the engine does not know, so a typo is
        // visible instead of silently doing nothing.
        for (const auto& [name, value] : actions.items()) {
            bool known = false;
            for (std::size_t i = 0; i < kActionCount && !known; ++i) {
                known = name == ToString(static_cast<Action>(i));
            }
            if (!known) {
                LOG_WARN("Input bindings: unknown action '%s' ignored", name.c_str());
            }
        }

        LOG_INFO("Loaded input bindings from %s", path.c_str());
        return true;
    }

    bool InputMap::IsDown(const Input& input, const Action action) const
    {
        const auto& bindings = m_bindings[static_cast<std::size_t>(action)];

        return std::ranges::any_of(
            bindings,
            [&input](const Scancode scancode) {
                return input.IsScancodeDown(scancode);
            });
    }

    bool InputMap::WasPressed(const Input& input, const Action action) const
    {
        const auto& bindings = m_bindings[static_cast<std::size_t>(action)];

        return std::ranges::any_of(
            bindings,
            [&input](const Scancode scancode)
            {
                return input.WasScancodePressed(scancode);
            }
        );
    }

} // namespace engine
