#pragma once

#include "chronos/core/event_bus.h"
#include "chronos/observability/log.h"

namespace chronos {

/// Subscribes to an EventBus and renders every event as a structured log
/// line. RAII: unsubscribes on destruction.
///
/// This is the pattern all observability follows -- the metrics registry
/// and timeline recorder (Phase 4) are just more subscribers. The core
/// never knows logging exists.
class EventLogger {
public:
    explicit EventLogger(EventBus& bus)
        : bus_(bus),
          subscription_(bus.subscribe([](const Event& e) { log_event(e); })) {}

    ~EventLogger() { bus_.unsubscribe(subscription_); }

    EventLogger(const EventLogger&) = delete;
    EventLogger& operator=(const EventLogger&) = delete;

private:
    static void log_event(const Event& e) {
        switch (e.type) {
            case EventType::JobSubmitted:
                log::info("job submitted", {{"job_id", e.job_id.value()},
                                            {"detail", e.detail}});
                break;
            case EventType::JobStateChanged:
                log::info("job state changed", {{"job_id", e.job_id.value()},
                                                {"from", to_string(e.from_state)},
                                                {"to", to_string(e.to_state)},
                                                {"attempt", e.attempt}});
                break;
            case EventType::WorkerRegistered:
                log::info("worker registered", {{"worker_id", e.worker_id.value()},
                                                {"detail", e.detail}});
                break;
            case EventType::WorkerMarkedDead:
                log::warn("worker marked dead", {{"worker_id", e.worker_id.value()},
                                                 {"detail", e.detail}});
                break;
        }
    }

    EventBus& bus_;
    EventBus::SubscriptionId subscription_;
};

}  // namespace chronos
