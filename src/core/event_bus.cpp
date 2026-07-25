#include "chronos/core/event_bus.h"

#include <algorithm>

namespace chronos {

EventBus::SubscriptionId EventBus::subscribe(Handler handler) {
    const SubscriptionId id = next_id_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(mutex_);
    handlers_.emplace_back(id, std::move(handler));
    return id;
}

void EventBus::unsubscribe(SubscriptionId id) {
    std::unique_lock lock(mutex_);
    std::erase_if(handlers_, [id](const auto& entry) { return entry.first == id; });
}

void EventBus::publish(const Event& event) const {
    // Snapshot the handler list under a shared lock, then invoke outside it.
    // This lets handlers re-enter the bus (subscribe/publish) without
    // deadlock, at the documented cost that a handler racing with its own
    // unsubscribe may see one final event.
    std::vector<Handler> snapshot;
    {
        std::shared_lock lock(mutex_);
        snapshot.reserve(handlers_.size());
        for (const auto& [id, handler] : handlers_) {
            snapshot.push_back(handler);
        }
    }
    for (const auto& handler : snapshot) {
        handler(event);
    }
}

std::size_t EventBus::subscriber_count() const {
    std::shared_lock lock(mutex_);
    return handlers_.size();
}

}  // namespace chronos
