// chronos-bench: measured performance of the real system.
//
//   1. End-to-end throughput: N no-op jobs through the full stack
//      (scheduler thread + worker threads + heartbeats), wall-clocked.
//   2. Scheduling latency: Queued -> Dispatched percentiles from the
//      metrics pipeline during that run.
//   3. ReadySet microbenchmark: add / best_where / remove ops per second.

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "chronos/core/event_bus.h"
#include "chronos/core/job_store.h"
#include "chronos/execution/local_thread_backend.h"
#include "chronos/observability/metrics.h"
#include "chronos/observability/scheduler_metrics.h"
#include "chronos/scheduling/policies.h"
#include "chronos/scheduling/ready_set.h"
#include "chronos/scheduling/scheduler.h"

using namespace chronos;
using namespace std::chrono_literals;

namespace {

void bench_end_to_end(int jobs, int workers_n) {
    SystemClock clock;
    EventBus bus;
    JobStore store(clock, bus);
    WorkerRegistry registry(clock, bus);
    MetricsRegistry metrics;
    SchedulerMetrics scheduler_metrics(bus, metrics);

    Scheduler scheduler(store, registry, bus, clock,
                        std::make_unique<CompositePolicy>(),
                        RetryConfig{.jitter = 0.0});
    std::vector<ResourceCapacity> caps(
        static_cast<std::size_t>(workers_n),
        ResourceCapacity{.cpu_units = 4, .memory_mb = 1024});
    LocalThreadBackend backend(
        registry, scheduler, clock,
        [](const Job&) { return ExecutionResult{.success = true, .error = {}}; },
        caps, /*heartbeat_interval=*/50ms);
    scheduler.attach_backend(backend);
    backend.start();
    scheduler.start();

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < jobs; ++i) {
        scheduler.submit(JobSpecBuilder{}
                             .priority(i % 10)
                             .resources({.cpu_units = 1, .memory_mb = 64})
                             .build());
    }
    scheduler.stop(/*drain=*/true);
    const double sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    backend.stop();

    auto& latency = metrics.histogram("chronos_scheduling_latency_seconds", "");
    std::printf("end-to-end       : %d no-op jobs, %d workers -> %.2f s "
                "(%.0f jobs/s sustained)\n",
                jobs, workers_n, sec, jobs / sec);
    std::printf("sched latency    : p50 %.2f ms | p95 %.2f ms | p99 %.2f ms "
                "(%llu queue visits)\n",
                latency.percentile(0.50) * 1e3, latency.percentile(0.95) * 1e3,
                latency.percentile(0.99) * 1e3,
                static_cast<unsigned long long>(latency.count()));
}

void bench_ready_set(int n) {
    PriorityPolicy policy;
    ReadySet set(policy);
    const TimePoint t0 = TimePoint{} + 100s;

    std::vector<Job> jobs;
    jobs.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        Job job;
        job.id = JobId{static_cast<std::uint64_t>(i + 1)};
        job.spec = JobSpecBuilder{}.priority(i % 100).build();
        job.submit_time = t0;
        jobs.push_back(job);
    }

    auto timer = [] { return std::chrono::steady_clock::now(); };

    auto t = timer();
    for (const Job& job : jobs) {
        set.add(job, t0);
    }
    const double add_s = std::chrono::duration<double>(timer() - t).count();

    t = timer();
    for (int i = 0; i < n; ++i) {
        (void)set.best_where([](JobId) { return true; }, 1);
    }
    const double best_s = std::chrono::duration<double>(timer() - t).count();

    t = timer();
    for (const Job& job : jobs) {
        set.remove(job.id);
    }
    const double rm_s = std::chrono::duration<double>(timer() - t).count();

    std::printf("ReadySet (n=%d)  : add %.1fM ops/s | best %.1fM ops/s | "
                "remove %.1fM ops/s\n",
                n, n / add_s / 1e6, n / best_s / 1e6, n / rm_s / 1e6);
}

}  // namespace

int main(int argc, char** argv) {
    int jobs = 5000;
    int workers = 4;
    if (argc > 1) {
        jobs = std::atoi(argv[1]);
    }
    if (argc > 2) {
        workers = std::atoi(argv[2]);
    }

    std::printf("chronos-bench (hardware threads: %u)\n\n",
                std::thread::hardware_concurrency());
    bench_end_to_end(jobs, workers);
    bench_ready_set(100000);
    return 0;
}
