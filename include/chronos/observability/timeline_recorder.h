#pragma once

#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "chronos/core/event_bus.h"

namespace chronos {

/// Keeps a bounded, queryable record of recent system events, plus a
/// per-job view. Backs the Phase-5 API (`GET /jobs/:id/timeline`,
/// `GET /events`) and the dashboard's live feed.
///
/// The global log is a ring (oldest evicted at capacity). Per-job views
/// share the cap indirectly: an evicted event also leaves its job's view,
/// so memory is bounded by max_events regardless of job count.
class TimelineRecorder {
public:
    explicit TimelineRecorder(EventBus& bus, std::size_t max_events = 10000)
        : bus_(bus), max_events_(max_events) {
        subscription_ = bus_.subscribe([this](const Event& e) { record(e); });
    }

    ~TimelineRecorder() { bus_.unsubscribe(subscription_); }

    TimelineRecorder(const TimelineRecorder&) = delete;
    TimelineRecorder& operator=(const TimelineRecorder&) = delete;

    /// Events for one job, oldest first (within the retained window).
    [[nodiscard]] std::vector<Event> for_job(JobId id) const {
        std::lock_guard lock(mutex_);
        std::vector<Event> out;
        for (const Event& e : events_) {
            if (e.job_id == id) {
                out.push_back(e);
            }
        }
        return out;
    }

    /// The most recent `n` events, oldest first.
    [[nodiscard]] std::vector<Event> recent(std::size_t n) const {
        std::lock_guard lock(mutex_);
        const std::size_t take = std::min(n, events_.size());
        return {events_.end() - static_cast<std::ptrdiff_t>(take), events_.end()};
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return events_.size();
    }

private:
    void record(const Event& e) {
        std::lock_guard lock(mutex_);
        events_.push_back(e);
        if (events_.size() > max_events_) {
            events_.pop_front();
        }
    }

    EventBus& bus_;
    EventBus::SubscriptionId subscription_;
    std::size_t max_events_;

    mutable std::mutex mutex_;
    std::deque<Event> events_;
};

}  // namespace chronos
