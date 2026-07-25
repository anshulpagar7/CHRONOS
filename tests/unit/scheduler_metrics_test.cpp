#include "chronos/observability/scheduler_metrics.h"

#include <gtest/gtest.h>

#include <chrono>

#include "chronos/core/job_store.h"

namespace chronos {
namespace {

using namespace std::chrono_literals;

class SchedulerMetricsTest : public ::testing::Test {
protected:
    SimulatedClock clock_{TimePoint{} + 1000s};
    EventBus bus_;
    JobStore store_{clock_, bus_};
    MetricsRegistry registry_;
    SchedulerMetrics metrics_{bus_, registry_};

    Counter& counter(const std::string& name) { return registry_.counter(name, ""); }
    Gauge& gauge(const std::string& name) { return registry_.gauge(name, ""); }
};

TEST_F(SchedulerMetricsTest, LifecycleUpdatesCountersGaugesAndLatencies) {
    const JobId id = store_.submit(JobSpecBuilder{}.build());
    EXPECT_EQ(counter("chronos_jobs_submitted_total").value(), 1u);

    store_.transition(id, JobState::Queued);
    EXPECT_EQ(gauge("chronos_jobs_queued").value(), 1);

    clock_.advance(3s);  // Queue wait: exactly 3s.
    store_.transition(id, JobState::Dispatched);
    EXPECT_EQ(gauge("chronos_jobs_queued").value(), 0);

    store_.transition(id, JobState::Running);
    EXPECT_EQ(gauge("chronos_jobs_running").value(), 1);

    clock_.advance(2s);  // Turnaround: 5s total.
    store_.transition(id, JobState::Completed);
    EXPECT_EQ(gauge("chronos_jobs_running").value(), 0);
    EXPECT_EQ(counter("chronos_jobs_completed_total").value(), 1u);

    auto& latency = registry_.histogram("chronos_scheduling_latency_seconds", "");
    EXPECT_EQ(latency.count(), 1u);
    EXPECT_DOUBLE_EQ(latency.sum(), 3.0);  // Exact, thanks to SimulatedClock.

    auto& turnaround = registry_.histogram("chronos_turnaround_seconds", "");
    EXPECT_EQ(turnaround.count(), 1u);
    EXPECT_DOUBLE_EQ(turnaround.sum(), 5.0);
}

TEST_F(SchedulerMetricsTest, RetryAndRescueAndTerminalCounters) {
    const JobId id = store_.submit(JobSpecBuilder{}.max_retries(1).build());
    store_.transition(id, JobState::Queued);
    store_.transition(id, JobState::Dispatched);
    store_.transition(id, JobState::Running);
    store_.transition(id, JobState::Queued);  // Rescue.
    EXPECT_EQ(counter("chronos_rescues_total").value(), 1u);

    store_.transition(id, JobState::Dispatched);
    store_.transition(id, JobState::Running);
    store_.transition(id, JobState::RetryWait);  // Failure -> backoff.
    EXPECT_EQ(counter("chronos_retries_total").value(), 1u);
    EXPECT_EQ(gauge("chronos_jobs_retry_wait").value(), 1);

    store_.transition(id, JobState::Queued);
    store_.transition(id, JobState::Dispatched);
    store_.transition(id, JobState::Running);
    store_.transition(id, JobState::Failed);  // Dead-letter.
    EXPECT_EQ(counter("chronos_jobs_failed_total").value(), 1u);
    EXPECT_EQ(gauge("chronos_jobs_retry_wait").value(), 0);
    EXPECT_EQ(gauge("chronos_jobs_running").value(), 0);

    const JobId c = store_.submit(JobSpecBuilder{}.build());
    store_.transition(c, JobState::Cancelled);
    EXPECT_EQ(counter("chronos_jobs_cancelled_total").value(), 1u);
}

TEST_F(SchedulerMetricsTest, PerQueueVisitLatencyIsRecordedEachVisit) {
    const JobId id = store_.submit(JobSpecBuilder{}.max_retries(2).build());
    store_.transition(id, JobState::Queued);
    clock_.advance(1s);
    store_.transition(id, JobState::Dispatched);
    store_.transition(id, JobState::Running);
    store_.transition(id, JobState::RetryWait);
    store_.transition(id, JobState::Queued);
    clock_.advance(4s);
    store_.transition(id, JobState::Dispatched);

    auto& latency = registry_.histogram("chronos_scheduling_latency_seconds", "");
    EXPECT_EQ(latency.count(), 2u);
    EXPECT_DOUBLE_EQ(latency.sum(), 5.0);  // 1s + 4s.
}

}  // namespace
}  // namespace chronos
