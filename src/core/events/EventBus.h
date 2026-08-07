#pragma once
#include <functional>
#include <typeindex>

namespace engine {

    class EventBus {

        public:
            template<typename T>
            using Handler = std::function<void(const T&)>;

            template<typename T>
            void Subscribe(Handler<T> handler)
            {
                auto wrapper = [handler = std::move(handler)](const void* event)
                {
                    handler(*static_cast<const T*>(event));
                };

                m_handlers[typeid(T)].push_back(std::move(wrapper));
            }

            template<typename T>
            void Emit(const T& event)
            {
                auto it = m_handlers.find(typeid(T));

                if (it == m_handlers.end())
                    return;

                for (auto& handler : it->second)
                    handler(&event);
            }

        private:
            using RawHandler = std::function<void(const void*)>;

            std::unordered_map<std::type_index, std::vector<RawHandler>> m_handlers;
    };
}
