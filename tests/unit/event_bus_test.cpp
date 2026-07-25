#include "chronos/core/event_bus.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace chronos {
namespace {

Event submitted_event(std::uint64_t job_id) {
    Event e{};
    e.type = EventType::JobSubmitted;
    e.job_id = JobId{job_id};
    return e;
}

TEST(EventBus, DeliversToSubscriber) {
    EventBus bus;
    std::vector<std::uint64_t> seen;
    bus.subscribe([&seen](const Event& e) { seen.push_back(e.job_id.value()); });

    bus.publish(submitted_event(1));
    bus.publish(submitted_event(2));

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], 1u);
    EXPECT_EQ(seen[1], 2u);
}

TEST(EventBus, DeliversToAllSubscribersInSubscriptionOrder) {
    EventBus bus;
    std::vector<int> order;
    bus.subscribe([&order](const Event&) { order.push_back(1); });
    bus.subscribe([&order](const Event&) { order.push_back(2); });
    bus.subscribe([&order](const Event&) { order.push_back(3); });

    bus.publish(submitted_event(1));

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(EventBus, UnsubscribeStopsDelivery) {
    EventBus bus;
    int count = 0;
    const auto id = bus.subscribe([&count](const Event&) { ++count; });

    bus.publish(submitted_event(1));
    EXPECT_EQ(count, 1);

    bus.unsubscribe(id);
    bus.publish(submitted_event(2));
    EXPECT_EQ(count, 1);
    EXPECT_EQ(bus.subscriber_count(), 0u);
}

TEST(EventBus, UnsubscribeUnknownIdIsIgnored) {
    EventBus bus;
    bus.unsubscribe(9999);  // Must not throw or crash.
    EXPECT_EQ(bus.subscriber_count(), 0u);
}

TEST(EventBus, HandlerMaySubscribeDuringPublish) {
    // Reentrancy: a handler that touches the bus must not deadlock.
    EventBus bus;
    int late_count = 0;
    bool registered = false;

    bus.subscribe([&](const Event&) {
        if (!registered) {
            registered = true;
            bus.subscribe([&late_count](const Event&) { ++late_count; });
        }
    });

    bus.publish(submitted_event(1));  // Registers the late subscriber.
    bus.publish(submitted_event(2));  // Late subscriber sees this one.

    EXPECT_EQ(late_count, 1);
    EXPECT_EQ(bus.subscriber_count(), 2u);
}

TEST(EventBus, ConcurrentPublishersDeliverEveryEvent) {
    EventBus bus;
    std::atomic<int> delivered{0};
    bus.subscribe([&delivered](const Event&) {
        delivered.fetch_add(1, std::memory_order_relaxed);
    });

    constexpr int kThreads = 4;
    constexpr int kEventsPerThread = 250;

    std::vector<std::thread> publishers;
    publishers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        publishers.emplace_back([&bus] {
            for (int i = 0; i < kEventsPerThread; ++i) {
                bus.publish(submitted_event(1));
            }
        });
    }
    for (auto& t : publishers) {
        t.join();
    }

    EXPECT_EQ(delivered.load(), kThreads * kEventsPerThread);
}

}  // namespace
}  // namespace chronos
