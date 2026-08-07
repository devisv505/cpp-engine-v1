#include "TestSupport.h"

#include <string>
#include <utility>
#include <vector>

#include "core/events/EventBus.h"
#include "core/events/Events.h"

using namespace engine;
using tests::TestRun;

namespace {

    struct Ping { int value = 0; };
    struct Pong { };
    template<int N> struct Filler { };

    // Subscribing to previously unseen types inserts into the handler map,
    // which is what can rehash it mid-dispatch.
    template<int... Is>
    void SubscribeNewTypes(EventBus& bus, std::vector<Subscription>& keep,
                           std::integer_sequence<int, Is...>)
    {
        (keep.push_back(bus.Subscribe<Filler<Is>>([](const Filler<Is>&) {})), ...);
    }

} // namespace

int main()
{
    TestRun t("event bus");

    {
        EventBus bus;
        int      received = 0;
        auto     sub = bus.Subscribe<Ping>([&](const Ping& p) { received = p.value; });
        bus.Emit(Ping{42});
        CHECK(t, "handler receives its event", received == 42);
        bus.Emit(Pong{});
        CHECK(t, "a different type does not fire it", received == 42);
    }

    {
        EventBus    bus;
        std::string order;
        auto a = bus.Subscribe<Ping>([&](const Ping&) { order += "a"; });
        auto b = bus.Subscribe<Ping>([&](const Ping&) { order += "b"; });
        auto c = bus.Subscribe<Ping>([&](const Ping&) { order += "c"; });
        bus.Emit(Ping{});
        CHECK(t, "handlers run in subscription order", order == "abc");
        CHECK(t, "HandlerCount counts the living", bus.HandlerCount<Ping>() == 3);
    }

    {   // The point of the RAII handle: no dangling captured `this`.
        EventBus bus;
        int      calls = 0;
        {
            auto sub = bus.Subscribe<Ping>([&](const Ping&) { ++calls; });
            bus.Emit(Ping{});
        }
        bus.Emit(Ping{});
        CHECK(t, "handler stops when its Subscription dies", calls == 1);
        CHECK(t, "the dead entry is compacted away", bus.HandlerCount<Ping>() == 0);
    }

    {
        EventBus bus;
        int      calls = 0;
        auto     outer = bus.Subscribe<Ping>([&](const Ping&) { ++calls; });
        {
            Subscription moved = std::move(outer);
            bus.Emit(Ping{});
            CHECK(t, "moved-to handle keeps the subscription alive", calls == 1);
            CHECK(t, "moved-from handle is inert", !outer.IsActive());
        }
        bus.Emit(Ping{});
        CHECK(t, "moving transfers ownership rather than duplicating", calls == 1);
    }

    {
        EventBus bus;
        int      calls = 0;
        auto     sub = bus.Subscribe<Ping>([&](const Ping&) { ++calls; });
        sub.Reset();
        bus.Emit(Ping{});
        CHECK(t, "Reset unsubscribes immediately", calls == 0);
        sub.Reset();
        CHECK(t, "Reset twice is harmless", true);
    }

    {   // Re-entrancy: subscribing to the SAME type reallocates the vector.
        EventBus                  bus;
        int                       outer = 0, inner = 0;
        std::vector<Subscription> keep;
        auto sub = bus.Subscribe<Ping>([&](const Ping&) {
            ++outer;
            if (keep.empty()) {
                for (int i = 0; i < 50; ++i) {
                    keep.push_back(bus.Subscribe<Ping>([&](const Ping&) { ++inner; }));
                }
            }
        });
        bus.Emit(Ping{});
        CHECK(t, "subscribing during dispatch is safe", outer == 1);
        CHECK(t, "...new handlers do not run in the same emit", inner == 0);
        bus.Emit(Ping{});
        CHECK(t, "...but do run on the next", inner == 50);
    }

    {   // Re-entrancy: subscribing to a NEW type can rehash the handler map,
        // which invalidates iterators (references survive).
        EventBus                  bus;
        std::vector<Subscription> keep;
        int                       calls = 0;
        auto sub = bus.Subscribe<Ping>([&](const Ping&) {
            ++calls;
            SubscribeNewTypes(bus, keep, std::make_integer_sequence<int, 64>{});
        });
        bus.Emit(Ping{});
        bus.Emit(Ping{});
        CHECK(t, "subscribing new event types during dispatch is safe",
              calls == 2 && keep.size() == 128);
    }

    {
        EventBus     bus;
        int          first = 0, second = 0;
        Subscription later;
        auto sub = bus.Subscribe<Ping>([&](const Ping&) { ++first; later.Reset(); });
        later    = bus.Subscribe<Ping>([&](const Ping&) { ++second; });
        bus.Emit(Ping{});
        CHECK(t, "a handler removed mid-dispatch is not called", first == 1 && second == 0);
    }

    {
        EventBus bus;
        int      pings = 0, pongs = 0;
        auto p = bus.Subscribe<Ping>([&](const Ping&) { ++pings; bus.Emit(Pong{}); });
        auto q = bus.Subscribe<Pong>([&](const Pong&) { ++pongs; });
        bus.Emit(Ping{});
        CHECK(t, "emitting from inside a handler works", pings == 1 && pongs == 1);
    }

    {
        EventBus   bus;
        Vector2Int seen{};
        auto sub = bus.Subscribe<WindowResized>([&](const WindowResized& e) { seen = e.size; });
        bus.Emit(WindowResized{{1728, 977}});
        CHECK(t, "engine event types carry their payload", seen == (Vector2Int{1728, 977}));
    }

    {
        EventBus bus;
        int      calls = 0;
        auto     sub = bus.Subscribe<Ping>([&](const Ping&) { ++calls; });
        bus.Clear();
        bus.Emit(Ping{});
        CHECK(t, "Clear drops every handler", calls == 0);
        sub.Reset();
        CHECK(t, "Reset after Clear is safe", true);
    }

    {   // Lifetime: a Subscription may outlive its bus. The weak reference
        // expires with the bus, so Reset and the destructor become no-ops.
        Subscription sub;
        {
            EventBus bus;
            sub = bus.Subscribe<Ping>([](const Ping&) {});
            CHECK(t, "subscription is active while the bus lives", sub.IsActive());
        }
        CHECK(t, "subscription expires when the bus dies", !sub.IsActive());
        sub.Reset();
        CHECK(t, "Reset after the bus is gone is harmless", true);

        Subscription dies_after_bus;
        {
            EventBus bus;
            dies_after_bus = bus.Subscribe<Ping>([](const Ping&) {});
        }
        CHECK(t, "destruction after the bus is gone is harmless", true);
    }   // ~Subscription runs here, long after its bus

    {   // Re-entrancy: a dead entry plus a nested emit of the SAME type. The
        // nested Emit must not compact the list the outer Emit is walking.
        EventBus bus;
        int      aCalls = 0, cCalls = 0;
        bool     nested = false;
        auto dead = bus.Subscribe<Ping>([](const Ping&) {});
        auto a    = bus.Subscribe<Ping>([&](const Ping&) {
            ++aCalls;
            if (!nested) {
                nested = true;
                bus.Emit(Ping{});
            }
        });
        auto c = bus.Subscribe<Ping>([&](const Ping&) { ++cCalls; });
        dead.Reset();  // leaves a dead entry at the front of the list
        bus.Emit(Ping{});
        CHECK(t, "nested same-type emit does not corrupt the outer dispatch",
              aCalls == 2 && cCalls == 2);
        CHECK(t, "the dead entry is compacted once dispatch fully ends",
              bus.HandlerCount<Ping>() == 2);
    }

    return t.Result();
}
