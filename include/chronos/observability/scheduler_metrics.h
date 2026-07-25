#pragma once

#include <mutex>
#include <unordered_map>

#include "chronos/core/clock.h"
#include "chronos/core/event_bus.h"
#include "chronos/observability/metrics.h"

namespace chronos {

/// Translates the event stream into operational metrics. Pure subscriber:
/// the scheduler and store don't know it exists (ADR-002 pays off again).
///
/// Exposes (Prometheus names):
///   chronos_jobs_submitted_total / completed_total / failed_total /
///     cancelled_total / retries_total / rescues_total          (counters)
///   chronos_workers_registered_total / workers_dead_total      (counters)
///   chronos_jobs_queued / running / retry_wait                 (gauges)
///   chronos_scheduling_latency_seconds   Queued -> Dispatched  (histogram)
///   chronos_turnaround_seconds           submit -> Completed   (histogram)
///
/// Latencies are computed from event timestamps, so they are correct under
/// both the real clock and the simulator's clock.
class SchedulerMetrics {
public:
    SchedulerMetrics(EventBus& bus, MetricsRegistry& registry);
    ~SchedulerMetrics();

    SchedulerMetrics(const SchedulerMetrics&) = delete;
    SchedulerMetrics& operator=(const SchedulerMetrics&) = delete;

private:
    void on_event(const Event& e);
    void adjust_state_gauges(JobState from, JobState to);

    EventBus& bus_;
    EventBus::SubscriptionId subscription_;

    Counter& submitted_;
    Counter& completed_;
    Counter& failed_;
    Counter& cancelled_;
    Counter& retries_;
    Counter& rescues_;
    Counter& workers_registered_;
    Counter& workers_dead_;
    Gauge& queued_;
    Gauge& running_;
    Gauge& retry_wait_;
    Histogram& scheduling_latency_;
    Histogram& turnaround_;

    std::mutex mutex_;
    std::unordered_map<JobId, TimePoint> submit_ts_;
    std::unordered_map<JobId, TimePoint> queued_ts_;
};

}  // namespace chronos
