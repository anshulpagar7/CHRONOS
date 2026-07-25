#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "chronos/core/event.h"

namespace chronos {

/// Synchronous, thread-safe publish/subscribe bus.
///
/// One mechanism feeds three features: the structured logger, the metrics
/// registry, and the execution-timeline recorder are all just subscribers.
///
/// Delivery is synchronous and in subscription order, on the publisher's
/// thread. This is deliberate: it keeps event ordering deterministic (vital
/// for the simulator) and makes causality trivial to reason about. The
/// trade-off -- a slow subscriber blocks the publisher -- is acceptable
/// because all built-in subscribers are O(1); an async sink can be layered
/// on top later without changing this interface.
/// See docs/decisions/ADR-002-synchronous-event-bus.md.
///
/// Concurrency notes:
///  * subscribe/unsubscribe/publish are all safe to call from any thread.
///  * publish snapshots the handler list, then invokes handlers *outside*
///    the lock, so a handler may re-enter the bus without deadlocking.
///  * Consequence: a handler racing with its own unsubscribe may receive
///    one final event after unsubscribe returns.
class EventBus {
public:
    using Handler = std::function<void(const Event&)>;
    using SubscriptionId = std::uint64_t;

    /// Register a handler. Returns a token for unsubscribe().
    SubscriptionId subscribe(Handler handler);

    /// Remove a previously registered handler. Unknown ids are ignored.
    void unsubscribe(SubscriptionId id);

    /// Deliver `event` to all current subscribers, synchronously.
    void publish(const Event& event) const;

    [[nodiscard]] std::size_t subscriber_count() const;

private:
    mutable std::shared_mutex mutex_;
    std::vector<std::pair<SubscriptionId, Handler>> handlers_;
    std::atomic<SubscriptionId> next_id_{1};
};

}  // namespace chronos
