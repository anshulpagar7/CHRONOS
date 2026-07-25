#include "chronos/observability/scheduler_metrics.h"

#include <chrono>

namespace chronos {

namespace {

double seconds_between(TimePoint from, TimePoint to) {
    return std::chrono::duration<double>(to - from).count();
}

}  // namespace

SchedulerMetrics::SchedulerMetrics(EventBus& bus, MetricsRegistry& registry)
    : bus_(bus),
      subscription_(0),
      submitted_(registry.counter("chronos_jobs_submitted_total",
                                  "Jobs accepted into the system")),
      completed_(registry.counter("chronos_jobs_completed_total",
                                  "Jobs finished successfully")),
      failed_(registry.counter("chronos_jobs_failed_total",
                               "Jobs dead-lettered after exhausting retries")),
      cancelled_(registry.counter("chronos_jobs_cancelled_total", "Jobs cancelled")),
      retries_(registry.counter("chronos_retries_total",
                                "Failures that entered retry backoff")),
      rescues_(registry.counter("chronos_rescues_total",
                                "Jobs rescued from dead workers mid-run")),
      workers_registered_(registry.counter("chronos_workers_registered_total",
                                           "Workers ever registered")),
      workers_dead_(registry.counter("chronos_workers_dead_total",
                                     "Workers declared dead by the lease monitor")),
      queued_(registry.gauge("chronos_jobs_queued", "Jobs currently queued")),
      running_(registry.gauge("chronos_jobs_running", "Jobs currently executing")),
      retry_wait_(registry.gauge("chronos_jobs_retry_wait",
                                 "Jobs waiting out retry backoff")),
      scheduling_latency_(registry.histogram(
          "chronos_scheduling_latency_seconds",
          "Time from entering the queue to dispatch (per queue visit)")),
      turnaround_(registry.histogram("chronos_turnaround_seconds",
                                     "Time from submission to successful completion")) {
    subscription_ = bus_.subscribe([this](const Event& e) { on_event(e); });
}

SchedulerMetrics::~SchedulerMetrics() {
    bus_.unsubscribe(subscription_);
}

void SchedulerMetrics::adjust_state_gauges(JobState from, JobState to) {
    const auto adjust = [&](JobState s, std::int64_t delta) {
        switch (s) {
            case JobState::Queued:    queued_.add(delta);     break;
            case JobState::Running:   running_.add(delta);    break;
            case JobState::RetryWait: retry_wait_.add(delta); break;
            default: break;
        }
    };
    adjust(from, -1);
    adjust(to, +1);
}

void SchedulerMetrics::on_event(const Event& e) {
    switch (e.type) {
        case EventType::JobSubmitted: {
            submitted_.inc();
            std::lock_guard lock(mutex_);
            submit_ts_[e.job_id] = e.timestamp;
            return;
        }
        case EventType::WorkerRegistered:
            workers_registered_.inc();
            return;
        case EventType::WorkerMarkedDead:
            workers_dead_.inc();
            return;
        case EventType::JobStateChanged:
            break;
    }

    adjust_state_gauges(e.from_state, e.to_state);

    switch (e.to_state) {
        case JobState::Queued: {
            std::lock_guard lock(mutex_);
            queued_ts_[e.job_id] = e.timestamp;
            if (e.from_state == JobState::Running) {
                rescues_.inc();
            }
            return;
        }
        case JobState::Dispatched: {
            std::lock_guard lock(mutex_);
            if (const auto it = queued_ts_.find(e.job_id); it != queued_ts_.end()) {
                scheduling_latency_.record(seconds_between(it->second, e.timestamp));
                queued_ts_.erase(it);
            }
            return;
        }
        case JobState::RetryWait:
            retries_.inc();
            return;
        case JobState::Completed: {
            completed_.inc();
            std::lock_guard lock(mutex_);
            if (const auto it = submit_ts_.find(e.job_id); it != submit_ts_.end()) {
                turnaround_.record(seconds_between(it->second, e.timestamp));
            }
            submit_ts_.erase(e.job_id);
            queued_ts_.erase(e.job_id);
            return;
        }
        case JobState::Failed:
        case JobState::Cancelled: {
            (e.to_state == JobState::Failed ? failed_ : cancelled_).inc();
            std::lock_guard lock(mutex_);
            submit_ts_.erase(e.job_id);  // Terminal: drop tracking state.
            queued_ts_.erase(e.job_id);
            return;
        }
        default:
            return;
    }
}

}  // namespace chronos
