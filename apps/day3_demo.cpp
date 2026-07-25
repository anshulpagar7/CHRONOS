// Day 3 demo: the full engine, end to end, with a mid-flight worker crash.
//
//   * 3 workers (one big, two small) execute jobs with simulated durations
//   * one job is flaky and needs a retry to succeed
//   * worker 2 is hard-killed while executing -- the heartbeat monitor
//     detects the death and rescues its job onto a survivor
//   * shutdown drains: every job reaches a terminal state first
//
// Every line of output below is the EventLogger rendering real system
// events -- nothing here is printf-narration.

#include <atomic>
#include <chrono>
#include <thread>

#include "chronos/core/event_bus.h"
#include "chronos/core/job_store.h"
#include "chronos/execution/local_thread_backend.h"
#include "chronos/observability/event_logger.h"
#include "chronos/observability/log.h"
#include "chronos/scheduling/policies.h"
#include "chronos/scheduling/scheduler.h"

using namespace chronos;
using namespace std::chrono_literals;

int main() {
    SystemClock clock;
    EventBus bus;
    JobStore store(clock, bus);
    WorkerRegistry registry(clock, bus);
    EventLogger event_logger(bus);

    SchedulerConfig config;
    config.heartbeat_timeout = 400ms;  // Snappy death detection for the demo.

    Scheduler scheduler(store, registry, bus, clock,
                        std::make_unique<CompositePolicy>(),
                        RetryConfig{.base_backoff = 200ms, .jitter = 0.1},
                        config);

    // Workers simulate work by sleeping; the "victim" job runs long enough
    // to be executing when we pull the plug on its worker.
    const JobExecutor executor = [](const Job& job) -> ExecutionResult {
        if (job.spec.name == "victim") {
            std::this_thread::sleep_for(2s);
        } else {
            std::this_thread::sleep_for(150ms);
        }
        if (job.spec.name == "flaky-etl" && job.attempt == 1) {
            return {.success = false, .error = "transient upstream timeout"};
        }
        return {.success = true, .error = {}};
    };

    LocalThreadBackend backend(registry, scheduler, clock, executor,
                               {{.cpu_units = 4, .memory_mb = 1024},
                                {.cpu_units = 2, .memory_mb = 512},
                                {.cpu_units = 2, .memory_mb = 512}},
                               /*heartbeat_interval=*/100ms);
    scheduler.attach_backend(backend);

    backend.start();
    scheduler.start();

    // A small mixed workload.
    scheduler.submit(JobSpecBuilder{}.name("victim").priority(8)
                         .resources({.cpu_units = 2, .memory_mb = 256}).build());
    scheduler.submit(JobSpecBuilder{}.name("flaky-etl").priority(5).max_retries(3)
                         .resources({.cpu_units = 1, .memory_mb = 128}).build());
    for (int i = 1; i <= 4; ++i) {
        scheduler.submit(JobSpecBuilder{}.name("batch-" + std::to_string(i))
                             .priority(i)
                             .resources({.cpu_units = 1, .memory_mb = 64}).build());
    }

    // Let execution get going, then crash whichever worker runs "victim".
    std::this_thread::sleep_for(300ms);
    for (const auto& w : registry.snapshot()) {
        for (const JobId running : w.running_jobs) {
            if (store.get(running)->spec.name == "victim") {
                backend.kill_worker(w.id);
            }
        }
    }

    // Drain: returns only when every job is terminal.
    scheduler.stop(/*drain=*/true);
    backend.stop();

    log::info("final state",
              {{"completed", static_cast<std::uint64_t>(store.count(JobState::Completed))},
               {"failed", static_cast<std::uint64_t>(store.count(JobState::Failed))},
               {"workers_alive", static_cast<std::uint64_t>(registry.alive_count())}});
    return 0;
}
