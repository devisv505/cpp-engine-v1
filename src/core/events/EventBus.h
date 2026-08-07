#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {

    namespace detail {

        // Shared between a bus and its Subscriptions. The bus owns it through a
        // shared_ptr; Subscriptions hold weak_ptrs, so a Subscription that
        // outlives its bus finds the state expired instead of touching freed
        // memory. Destruction order between bus and subscribers stops mattering.
        struct EventBusState {
            using RawHandler = std::function<void(const void*)>;

            struct Entry {
                std::uint64_t id = 0;
                RawHandler    handler;  // empty once removed
            };

            std::unordered_map<std::type_index, std::vector<Entry>> handlers;
            std::uint64_t nextId    = 0;
            int           emitDepth = 0;

            // Marks the entry dead rather than erasing it, so this is safe to
            // call from inside a handler while Emit is iterating.
            void Remove(const std::type_index type, const std::uint64_t id)
            {
                const auto it = handlers.find(type);
                if (it == handlers.end()) {
                    return;
                }
                for (Entry& entry : it->second) {
                    if (entry.id == id) {
                        entry.handler = nullptr;
                        return;
                    }
                }
            }
        };

    } // namespace detail

    // Keeps a subscription alive. When it is destroyed the handler is removed,
    // so a handler that captured `this` cannot outlive its owner: hold the
    // Subscription as a member of the subscriber.
    //
    // Holds only a weak reference to the bus, so it may safely outlive it:
    // resetting or destroying a Subscription after its bus is gone is a no-op.
    //
    // Move-only. A default-constructed or moved-from Subscription is inert.
    class Subscription {
        public:
            Subscription() = default;
            ~Subscription() { Reset(); }

            Subscription(Subscription&& other) noexcept
                : m_state(std::move(other.m_state))
                , m_type(other.m_type)
                , m_id(std::exchange(other.m_id, 0))
            {}

            Subscription& operator=(Subscription&& other) noexcept
            {
                if (this != &other) {
                    Reset();
                    m_state = std::move(other.m_state);
                    m_type  = other.m_type;
                    m_id    = std::exchange(other.m_id, 0);
                }
                return *this;
            }

            Subscription(const Subscription&) = delete;
            Subscription& operator=(const Subscription&) = delete;

            // Unsubscribes now instead of at end of scope. Harmless if the bus
            // is already gone.
            void Reset()
            {
                if (const auto state = m_state.lock()) {
                    state->Remove(m_type, m_id);
                }
                m_state.reset();
                m_id = 0;
            }

            // True while the handler is registered on a live bus.
            [[nodiscard]] bool IsActive() const { return !m_state.expired(); }

        private:
            friend class EventBus;
            Subscription(std::weak_ptr<detail::EventBusState> state,
                         const std::type_index type, const std::uint64_t id)
                : m_state(std::move(state)), m_type(type), m_id(id) {}

            std::weak_ptr<detail::EventBusState> m_state;
            std::type_index m_type = typeid(void);
            std::uint64_t   m_id   = 0;
    };

    // Type-indexed publish/subscribe. Handlers run synchronously inside Emit,
    // in subscription order. Single-threaded: subscribe and emit only from the
    // thread that owns the bus.
    //
    // Safe to subscribe, unsubscribe or emit — including re-entrantly for the
    // type already being dispatched — from inside a handler: Emit walks a
    // snapshot, removal only marks an entry dead, and dead entries are erased
    // only when the outermost Emit finishes.
    class EventBus {
        public:
            template<typename T>
            using Handler = std::function<void(const T&)>;

            EventBus() : m_state(std::make_shared<detail::EventBusState>()) {}

            // Subscriptions identify the bus they came from; copying would make
            // that ambiguous, so the bus stays put.
            EventBus(const EventBus&) = delete;
            EventBus& operator=(const EventBus&) = delete;

            // NOTE: discarding the result unsubscribes immediately — the
            // temporary Subscription is destroyed at the end of the statement.
            template<typename T>
            [[nodiscard]] Subscription Subscribe(Handler<T> handler)
            {
                const std::uint64_t id = ++m_state->nextId;

                auto wrapper = [handler = std::move(handler)](const void* event)
                {
                    handler(*static_cast<const T*>(event));
                };

                m_state->handlers[typeid(T)].push_back({id, std::move(wrapper)});
                return Subscription(m_state, typeid(T), id);
            }

            template<typename T>
            void Emit(const T& event)
            {
                auto& state = *m_state;
                const auto it = state.handlers.find(typeid(T));
                if (it == state.handlers.end()) {
                    return;
                }

                // Bind a reference, and never touch `it` again: a handler that
                // subscribes to a NEW event type inserts into the handler map,
                // which may rehash and invalidate every iterator. References to
                // elements survive a rehash; iterators do not.
                //
                // Index rather than iterate: a handler may also subscribe to
                // THIS type and reallocate the vector. The count is captured up
                // front so handlers added during this Emit run next time.
                auto& entries = it->second;
                {
                    const std::size_t count = entries.size();
                    const DepthGuard guard(state.emitDepth);
                    for (std::size_t i = 0; i < count; ++i) {
                        // Re-check each step: an earlier handler may have
                        // removed this one, and a dead entry must not be called.
                        if (entries[i].handler) {
                            entries[i].handler(&event);
                        }
                    }
                }

                // Only the outermost Emit compacts. A nested Emit of this same
                // type erasing entries would shift them under the outer loop,
                // whose captured count would then read past the end.
                if (state.emitDepth == 0) {
                    Compact(entries);
                }
            }

            template<typename T>
            [[nodiscard]] std::size_t HandlerCount() const
            {
                const auto it = m_state->handlers.find(typeid(T));
                if (it == m_state->handlers.end()) {
                    return 0;
                }
                std::size_t alive = 0;
                for (const Entry& entry : it->second) {
                    alive += entry.handler ? 1 : 0;
                }
                return alive;
            }

            // Drops every handler; outstanding Subscriptions become no-ops.
            // Must not be called from inside a handler: it destroys the list
            // Emit is walking.
            void Clear() const { m_state->handlers.clear(); }

        private:
            using Entry = detail::EventBusState::Entry;

            // Balances emitDepth even if a handler throws, so compaction is
            // not disabled for the rest of the bus's life.
            struct DepthGuard {
                explicit DepthGuard(int& depth) : m_depth(depth) { ++m_depth; }
                ~DepthGuard() { --m_depth; }
                int& m_depth;
            };

            // Drops dead entries. Only called once the outermost dispatch has
            // finished.
            static void Compact(std::vector<Entry>& entries)
            {
                std::erase_if(entries, [](const Entry& entry) { return !entry.handler; });
            }

            std::shared_ptr<detail::EventBusState> m_state;
    };

} // namespace engine
