#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "chronos/core/clock.h"
#include "chronos/core/job.h"
#include "chronos/execution/worker_registry.h"
#include "chronos/scheduling/scheduling_policy.h"

namespace chronos::sim {

/// Synthetic workload description. Everything is drawn from a seeded RNG,
/// so a (config, seed) pair defines one exact, reproducible workload.
struct WorkloadConfig {
    int jobs = 1000;
    std::uint64_t seed = 42;

    /// Poisson arrivals: exponential inter-arrival times at this rate.
    /// The default targets ~75% utilization of the default cluster
    /// (32 cpu / ~1.9 cpu-sec per job): feasible but contended, where
    /// scheduling policy actually matters. Crank it past ~16 to study
    /// overload behaviour instead.
    double arrival_rate_hz = 14.0;

    int priority_min = 0;
    int priority_max = 9;

    /// Fraction of jobs carrying a deadline, drawn uniformly in
    /// [deadline_min, deadline_max] after arrival.
    double deadline_fraction = 0.5;
    Duration deadline_min = std::chrono::seconds(2);
    Duration deadline_max = std::chrono::seconds(30);

    std::uint32_t cpu_min = 1;
    std::uint32_t cpu_max = 4;
    std::uint32_t mem_min = 64;
    std::uint32_t mem_max = 512;

    /// Uniform execution durations.
    Duration exec_min = std::chrono::milliseconds(50);
    Duration exec_max = std::chrono::milliseconds(1500);

    /// Per-attempt probability of failure; each job allows max_retries.
    double failure_prob = 0.08;
    int max_retries = 3;
};

struct ClusterConfig {
    std::vector<ResourceCapacity> workers = {
        {.cpu_units = 16, .memory_mb = 8192},
        {.cpu_units = 8, .memory_mb = 4096},
        {.cpu_units = 8, .memory_mb = 4096},
    };
};

/// Aggregated outcome of one simulation run.
struct SimResult {
    std::string policy;
    int jobs = 0;
    int completed = 0;
    int failed = 0;

    double sim_duration_sec = 0;   ///< Simulated time from t0 to last event.
    double wall_time_ms = 0;       ///< Real time the simulation took.
    double throughput_jobs_per_sec = 0;

    // Queue wait: submission -> first dispatch.
    double wait_p50_ms = 0;
    double wait_p95_ms = 0;
    double wait_p99_ms = 0;
    double wait_mean_ms = 0;

    double turnaround_mean_ms = 0;  ///< Submission -> terminal, completed jobs.

    int deadline_jobs = 0;    ///< Jobs that carried a deadline.
    int deadline_missed = 0;  ///< Deadline jobs not completed by it.
    double deadline_miss_rate = 0;

    /// Jain's fairness index over completed-job turnarounds, in (0, 1]:
    /// 1.0 = perfectly even treatment; lower = some jobs starved while
    /// others sailed through.
    double jain_fairness = 0;
};

/// Render a comparison table for several runs (same workload, different
/// policies) -- the output of `chronos-sim --compare`.
[[nodiscard]] std::string format_comparison(const std::vector<SimResult>& results);

/// Per-job CSV (one row per job) for external analysis / plotting.
[[nodiscard]] std::string result_csv_header();

/// Discrete-event simulation harness around the REAL scheduler.
///
/// No mocks of scheduling logic: the exact Scheduler/ReadySet/RetryManager/
/// WorkerRegistry production code runs, driven by run_once() on a
/// SimulatedClock (this is what ADR-001 and ADR-006 were building toward).
/// Only *execution* is simulated: dispatch schedules a completion event at
/// now + duration instead of running anything. The loop leaps the clock
/// from event to event, so ~10k jobs and hours of simulated time take
/// milliseconds of wall time.
///
/// Determinism: (workload, cluster, policy, seed) -> bit-identical results.
class Simulation {
public:
    using PolicyFactory = std::function<std::unique_ptr<SchedulingPolicy>()>;

    Simulation(WorkloadConfig workload, ClusterConfig cluster);

    /// Run one full simulation under `make_policy`. Reusable: each call
    /// builds a fresh system with the same seeded workload.
    SimResult run(const PolicyFactory& make_policy) const;

    /// Convenience: run all four built-in policies on the same workload.
    [[nodiscard]] std::vector<SimResult> compare() const;

private:
    WorkloadConfig workload_;
    ClusterConfig cluster_;
};

}  // namespace chronos::sim
