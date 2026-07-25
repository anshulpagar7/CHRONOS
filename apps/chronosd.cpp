// chronosd: the CHRONOS daemon. Boots the full system -- scheduler thread,
// local worker pool, metrics, timeline, REST API, dashboard -- and runs
// until interrupted.
//
//   chronosd --port=8080 --workers=3 --static-dir=dashboard --demo
//
// --demo keeps a live synthetic workload flowing (random priorities,
// sizes, deadlines, ~8% failures) so the dashboard has something to show.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>

#include "chronos/api/api_server.h"
#include "chronos/core/event_bus.h"
#include "chronos/core/job_store.h"
#include "chronos/execution/local_thread_backend.h"
#include "chronos/observability/log.h"
#include "chronos/observability/scheduler_metrics.h"
#include "chronos/scheduling/policies.h"

using namespace chronos;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) {
    g_stop.store(true);
}

bool parse_flag(std::string_view arg, std::string_view name, std::string& value) {
    if (arg.substr(0, name.size()) == name && arg.size() > name.size() &&
        arg[name.size()] == '=') {
        value = std::string(arg.substr(name.size() + 1));
        return true;
    }
    return false;
}

/// Background submitter for --demo mode.
void demo_loop(Scheduler& scheduler, Clock& clock, double rate_hz) {
    std::mt19937_64 rng(std::random_device{}());
    std::exponential_distribution<double> gap(rate_hz);
    std::uniform_int_distribution<int> priority(0, 9);
    std::uniform_int_distribution<std::uint32_t> cpu(1, 3);
    std::uniform_int_distribution<std::uint32_t> mem(64, 384);
    std::uniform_int_distribution<int> duration_ms(100, 1200);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const char* kinds[] = {"encode", "thumbnail", "report", "index",
                           "backup", "etl", "notify"};

    std::uint64_t n = 0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::duration<double>(gap(rng))));
        if (g_stop.load()) {
            return;
        }
        ++n;
        JobSpecBuilder builder;
        builder.name(std::string(kinds[n % 7]) + "-" + std::to_string(n))
            .priority(priority(rng))
            .max_retries(2)
            .resources({.cpu_units = cpu(rng), .memory_mb = mem(rng)})
            // The executor reads these back out of the payload.
            .payload("duration_ms=" + std::to_string(duration_ms(rng)) +
                     (unit(rng) < 0.08 ? ";fail" : ""));
        if (unit(rng) < 0.4) {
            builder.deadline(clock.now() + std::chrono::seconds(3 + (n % 20)));
        }
        scheduler.submit(builder.build());
    }
}

/// Demo executor: sleeps for the payload's duration, honours ";fail".
ExecutionResult demo_executor(const Job& job) {
    int duration_ms = 300;
    const std::string& payload = job.spec.payload;
    if (const auto pos = payload.find("duration_ms="); pos != std::string::npos) {
        duration_ms = std::atoi(payload.c_str() + pos + 12);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    // ";fail" fails the first attempt only, so retries visibly succeed.
    if (payload.find(";fail") != std::string::npos && job.attempt == 1) {
        return {.success = false, .error = "demo failure (first attempt)"};
    }
    return {.success = true, .error = {}};
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 8080;
    int workers = 3;
    std::string static_dir = "dashboard";
    bool demo = false;
    double demo_rate = 3.0;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::string value;
        if (arg == "--demo") {
            demo = true;
        } else if (parse_flag(arg, "--port", value)) {
            port = static_cast<std::uint16_t>(std::atoi(value.c_str()));
        } else if (parse_flag(arg, "--workers", value)) {
            workers = std::atoi(value.c_str());
        } else if (parse_flag(arg, "--static-dir", value)) {
            static_dir = value;
        } else if (parse_flag(arg, "--demo-rate", value)) {
            demo_rate = std::atof(value.c_str());
        } else {
            std::fprintf(stderr,
                         "usage: chronosd [--port=8080] [--workers=3]\n"
                         "                [--static-dir=dashboard] [--demo]\n"
                         "                [--demo-rate=3]\n");
            return 2;
        }
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    SystemClock clock;
    EventBus bus;
    JobStore store(clock, bus);
    WorkerRegistry registry(clock, bus);
    MetricsRegistry metrics;
    SchedulerMetrics scheduler_metrics(bus, metrics);
    TimelineRecorder timeline(bus, /*max_events=*/20000);

    Scheduler scheduler(store, registry, bus, clock,
                        std::make_unique<CompositePolicy>(),
                        RetryConfig{.base_backoff = 1s, .jitter = 0.2});

    std::vector<ResourceCapacity> capacities;
    for (int i = 0; i < workers; ++i) {
        capacities.push_back({.cpu_units = i == 0 ? 8u : 4u,
                              .memory_mb = i == 0 ? 4096u : 2048u});
    }
    LocalThreadBackend backend(registry, scheduler, clock, demo_executor,
                               capacities, /*heartbeat_interval=*/500ms);
    scheduler.attach_backend(backend);
    backend.start();
    scheduler.start();

    api::ApiServer server(scheduler, store, registry, metrics, timeline, clock,
                          port, static_dir);
    server.start();

    std::printf("chronosd up: http://localhost:%u  (dashboard: /  api: /api/state  "
                "metrics: /metrics)%s\n",
                server.port(), demo ? "  [demo workload on]" : "");

    std::thread demo_thread;
    if (demo) {
        demo_thread = std::thread([&scheduler, &clock, demo_rate] {
            demo_loop(scheduler, clock, demo_rate);
        });
    }

    while (!g_stop.load()) {
        std::this_thread::sleep_for(200ms);
    }

    std::printf("\nchronosd: shutting down\n");
    if (demo_thread.joinable()) {
        demo_thread.join();
    }
    server.stop();
    scheduler.stop(/*drain=*/false);
    backend.stop();
    return 0;
}
