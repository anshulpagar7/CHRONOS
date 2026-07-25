// Stress: the invariants that must survive volume + chaos.
//
// Throw a burst of jobs at real worker threads, kill a worker mid-flight,
// cancel a random slice, then drain -- and assert *conservation*: every
// job accounted for in exactly one terminal state, no stragglers stuck in
// transient states, and every alive worker's resources fully returned.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <thread>

#include "chronos/core/event_bus.h"
#include "chronos/core/job_store.h"
#include "chronos/execution/local_thread_backend.h"
#include "chronos/observability/metrics.h"
#include "chronos/observability/scheduler_metrics.h"
#include "chronos/scheduling/policies.h"
#include "chronos/scheduling/scheduler.h"

namespace chronos {
namespace {

using namespace std::chrono_literals;

TEST(Stress, ConservationUnderLoadChaosAndCancellation) {
    constexpr int kJobs = 1200;

    SystemClock clock;
    EventBus bus;
    JobStore store(clock, bus);
    WorkerRegistry registry(clock, bus);
    MetricsRegistry metrics;
    SchedulerMetrics scheduler_metrics(bus, metrics);

    SchedulerConfig config;
    config.heartbeat_timeout = 300ms;
    Scheduler scheduler(store, registry, bus, clock,
                        std::make_unique<CompositePolicy>(),
                        RetryConfig{.base_backoff = 5ms, .jitter = 0.0}, config);

    // Executor: brief real work; payload "flaky" fails the first attempt.
    LocalThreadBackend backend(
        registry, scheduler, clock,
        [](const Job& job) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            const bool flaky = job.spec.payload == "flaky" && job.attempt == 1;
            return ExecutionResult{.success = !flaky,
                                   .error = flaky ? "flaky" : ""};
        },
        {{.cpu_units = 8, .memory_mb = 2048},
         {.cpu_units = 4, .memory_mb = 1024},
         {.cpu_units = 4, .memory_mb = 1024}},
        /*heartbeat_interval=*/50ms);
    scheduler.attach_backend(backend);
    backend.start();
    scheduler.start();

    std::mt19937 rng(7);
    std::uniform_int_distribution<int> priority(0, 9);
    std::uniform_int_distribution<std::uint32_t> cpu(1, 3);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    std::vector<JobId> submitted;
    submitted.reserve(kJobs);
    for (int i = 0; i < kJobs; ++i) {
        submitted.push_back(scheduler.submit(
            JobSpecBuilder{}
                .priority(priority(rng))
                .max_retries(2)
                .resources({.cpu_units = cpu(rng), .memory_mb = 64})
                .payload(unit(rng) < 0.10 ? "flaky" : "steady")
                .build()));
        if (i == kJobs / 3) {
            // Chaos, mid-burst: crash a worker carrying live jobs.
            ASSERT_TRUE(backend.kill_worker(WorkerId{2}));
        }
        if (i % 9 == 0) {
            // Cancel a random earlier job; racing terminal states is fine
            // (cancel() may lose -- that's the point).
            (void)scheduler.cancel(
                submitted[std::uniform_int_distribution<std::size_t>(
                    0, submitted.size() - 1)(rng)]);
        }
    }

    scheduler.stop(/*drain=*/true);
    backend.stop();

    // ---- Conservation --------------------------------------------------
    int completed = 0, failed = 0, cancelled = 0, other = 0;
    for (const Job& job : store.snapshot()) {
        switch (job.state) {
            case JobState::Completed: ++completed; break;
            case JobState::Failed:    ++failed;    break;
            case JobState::Cancelled: ++cancelled; break;
            default:
                ++other;
                ADD_FAILURE() << "job " << job.id.value()
                              << " stuck in " << to_string(job.state);
        }
        // History sanity: first hop enters Queued, last hop is terminal.
        ASSERT_FALSE(job.history.empty());
        EXPECT_EQ(job.history.front().to, JobState::Queued);
        EXPECT_TRUE(is_terminal(job.history.back().to));
    }
    EXPECT_EQ(completed + failed + cancelled + other, kJobs);
    EXPECT_EQ(other, 0);
    EXPECT_GT(completed, kJobs / 2);  // The system did real work.

    // Flaky jobs retried: at least one visit through RetryWait happened.
    EXPECT_GT(metrics.counter("chronos_retries_total", "").value(), 0u);
    // The killed worker was noticed...
    EXPECT_EQ(metrics.counter("chronos_workers_dead_total", "").value(), 1u);

    // ---- Resource accounting -------------------------------------------
    int alive = 0;
    for (const WorkerInfo& w : registry.snapshot()) {
        if (!w.alive) {
            continue;
        }
        ++alive;
        EXPECT_EQ(w.available.cpu_units, w.total.cpu_units) << w.name;
        EXPECT_EQ(w.available.memory_mb, w.total.memory_mb) << w.name;
        EXPECT_TRUE(w.running_jobs.empty()) << w.name;
    }
    EXPECT_EQ(alive, 2);  // Three workers minus the one we crashed.
}

TEST(Stress, RepeatedStartStopCyclesLeakNothing) {
    // Lifecycle churn: bring the whole system up and down repeatedly with
    // work in flight. ASan/TSan turn any leak or race here into a failure.
    for (int cycle = 0; cycle < 5; ++cycle) {
        SystemClock clock;
        EventBus bus;
        JobStore store(clock, bus);
        WorkerRegistry registry(clock, bus);
        Scheduler scheduler(store, registry, bus, clock,
                            std::make_unique<FifoPolicy>(),
                            RetryConfig{.jitter = 0.0});
        LocalThreadBackend backend(
            registry, scheduler, clock,
            [](const Job&) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                return ExecutionResult{.success = true, .error = {}};
            },
            {{.cpu_units = 4, .memory_mb = 512}},
            /*heartbeat_interval=*/20ms);
        scheduler.attach_backend(backend);
        backend.start();
        scheduler.start();

        for (int i = 0; i < 40; ++i) {
            scheduler.submit(JobSpecBuilder{}
                                 .resources({.cpu_units = 1, .memory_mb = 32})
                                 .build());
        }
        // Odd cycles: drain cleanly. Even cycles: hard stop mid-flight.
        scheduler.stop(/*drain=*/cycle % 2 == 1);
        backend.stop();
    }
    SUCCEED();
}

}  // namespace
}  // namespace chronos
