// Integration tests: the full stack -- Scheduler thread + LocalThreadBackend
// worker threads + sidecar heartbeats -- on the real SystemClock. These are
// the tests TSan earns its keep on.
//
// Timing discipline: generous deadlines (seconds) with fast polling, never
// exact timing assertions -- those live in the deterministic unit tests.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "chronos/core/event_bus.h"
#include "chronos/core/job_store.h"
#include "chronos/execution/local_thread_backend.h"
#include "chronos/scheduling/policies.h"
#include "chronos/scheduling/scheduler.h"

namespace chronos {
namespace {

using namespace std::chrono_literals;

/// Poll until `pred` holds or `deadline` real time elapses.
template <typename Pred>
bool eventually(Pred&& pred, std::chrono::milliseconds deadline = 5000ms) {
    const auto give_up = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < give_up) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

struct Harness {
    explicit Harness(JobExecutor executor, std::vector<ResourceCapacity> capacities,
                     SchedulerConfig config = {}, RetryConfig retry = {.base_backoff = 20ms,
                                                                       .jitter = 0.0}) {
        scheduler = std::make_unique<Scheduler>(store, registry, bus, clock,
                                                std::make_unique<CompositePolicy>(),
                                                retry, config);
        backend = std::make_unique<LocalThreadBackend>(
            registry, *scheduler, clock, std::move(executor), std::move(capacities),
            /*heartbeat_interval=*/20ms);
        scheduler->attach_backend(*backend);
        backend->start();
        scheduler->start();
    }

    ~Harness() {
        scheduler->stop(/*drain=*/false);
        backend->stop();
    }

    SystemClock clock;
    EventBus bus;
    JobStore store{clock, bus};
    WorkerRegistry registry{clock, bus};
    std::unique_ptr<Scheduler> scheduler;
    std::unique_ptr<LocalThreadBackend> backend;
};

TEST(SchedulerIntegration, FiftyJobsAcrossThreeWorkersAllComplete) {
    std::atomic<int> executed{0};
    Harness h(
        [&executed](const Job&) {
            std::this_thread::sleep_for(2ms);
            executed.fetch_add(1);
            return ExecutionResult{.success = true, .error = {}};
        },
        {{.cpu_units = 2, .memory_mb = 256},
         {.cpu_units = 2, .memory_mb = 256},
         {.cpu_units = 4, .memory_mb = 512}});

    constexpr int kJobs = 50;
    for (int i = 0; i < kJobs; ++i) {
        h.scheduler->submit(JobSpecBuilder{}
                                .name("job-" + std::to_string(i))
                                .priority(i % 5)
                                .resources({.cpu_units = 1, .memory_mb = 64})
                                .build());
    }

    ASSERT_TRUE(eventually([&] { return h.store.count(JobState::Completed) == kJobs; }))
        << "completed=" << h.store.count(JobState::Completed);
    EXPECT_EQ(executed.load(), kJobs);
    EXPECT_EQ(h.scheduler->live_jobs(), 0u);

    // Every worker account balanced back to full.
    for (const auto& w : h.registry.snapshot()) {
        EXPECT_EQ(w.available.cpu_units, w.total.cpu_units) << w.name;
        EXPECT_TRUE(w.running_jobs.empty()) << w.name;
    }
}

TEST(SchedulerIntegration, FlakyJobsRetryToCompletion) {
    // Every job fails on its first attempt, succeeds on the second.
    std::atomic<int> first_failures{0};
    Harness h(
        [&first_failures](const Job& job) {
            if (job.attempt == 1) {
                first_failures.fetch_add(1);
                return ExecutionResult{.success = false, .error = "transient"};
            }
            return ExecutionResult{.success = true, .error = {}};
        },
        {{.cpu_units = 2, .memory_mb = 256}, {.cpu_units = 2, .memory_mb = 256}});

    constexpr int kJobs = 10;
    std::vector<JobId> ids;
    for (int i = 0; i < kJobs; ++i) {
        ids.push_back(h.scheduler->submit(
            JobSpecBuilder{}.max_retries(2)
                .resources({.cpu_units = 1, .memory_mb = 64}).build()));
    }

    ASSERT_TRUE(eventually([&] { return h.store.count(JobState::Completed) == kJobs; }));
    EXPECT_EQ(first_failures.load(), kJobs);
    for (const JobId id : ids) {
        const auto job = h.store.get(id);
        EXPECT_EQ(job->attempt, 2);  // Exactly one retry each.
    }
}

TEST(SchedulerIntegration, DrainStopWaitsForEverything) {
    Harness h(
        [](const Job&) {
            std::this_thread::sleep_for(5ms);
            return ExecutionResult{.success = true, .error = {}};
        },
        {{.cpu_units = 2, .memory_mb = 256}});

    for (int i = 0; i < 12; ++i) {
        h.scheduler->submit(
            JobSpecBuilder{}.resources({.cpu_units = 1, .memory_mb = 64}).build());
    }

    h.scheduler->stop(/*drain=*/true);  // Must block until all 12 finish.
    EXPECT_EQ(h.store.count(JobState::Completed), 12u);
    EXPECT_EQ(h.scheduler->live_jobs(), 0u);
}

TEST(SchedulerIntegration, KilledWorkerMidJobIsDetectedAndJobRescued) {
    SchedulerConfig config;
    config.heartbeat_timeout = 150ms;  // Fast death detection for the test.

    std::atomic<int> completions{0};
    Harness h(
        [&completions](const Job& job) {
            if (job.spec.name == "victim") {
                std::this_thread::sleep_for(80ms);  // Long enough to die during.
            } else {
                std::this_thread::sleep_for(2ms);
            }
            completions.fetch_add(1);
            return ExecutionResult{.success = true, .error = {}};
        },
        {{.cpu_units = 1, .memory_mb = 128}, {.cpu_units = 1, .memory_mb = 128}},
        config);

    // Occupy worker capacity so the victim's placement is observable.
    const JobId victim = h.scheduler->submit(
        JobSpecBuilder{}.name("victim").priority(9)
            .resources({.cpu_units = 1, .memory_mb = 64}).build());

    // Wait until the victim is actually running somewhere.
    ASSERT_TRUE(eventually(
        [&] { return h.store.get(victim)->state == JobState::Running; }));

    // Find and kill its worker mid-execution.
    WorkerId killed{};
    for (const auto& w : h.registry.snapshot()) {
        if (w.running_jobs.contains(victim)) {
            killed = w.id;
        }
    }
    ASSERT_TRUE(killed.valid());
    ASSERT_TRUE(h.backend->kill_worker(killed));

    // The heartbeat monitor must declare the worker dead, rescue the job,
    // and rerun it on the survivor -- through to completion.
    ASSERT_TRUE(eventually(
        [&] { return h.store.get(victim)->state == JobState::Completed; }));

    const auto dead = h.registry.get(killed);
    EXPECT_FALSE(dead->alive);

    // The job ran (at least started) twice: once on each worker. Its
    // timeline must contain a Running -> Queued rescue hop.
    const auto job = h.store.get(victim);
    EXPECT_GE(job->attempt, 2);
    bool rescued = false;
    for (const auto& hop : job->history) {
        if (hop.from == JobState::Running && hop.to == JobState::Queued) {
            rescued = true;
        }
    }
    EXPECT_TRUE(rescued);
}

}  // namespace
}  // namespace chronos
