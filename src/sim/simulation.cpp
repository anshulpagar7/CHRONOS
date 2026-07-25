#include "chronos/sim/simulation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <queue>
#include <random>

#include "chronos/core/event_bus.h"
#include "chronos/core/job_store.h"
#include "chronos/execution/execution_backend.h"
#include "chronos/scheduling/policies.h"
#include "chronos/scheduling/scheduler.h"

namespace chronos::sim {

namespace {

using namespace std::chrono_literals;

double to_ms(Duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

/// One pre-generated job: everything randomized is decided up front from
/// the seed, so every policy faces the *identical* workload.
struct PlannedJob {
    Duration arrival_offset;            ///< From simulation start.
    int priority;
    std::optional<Duration> deadline_offset;  ///< From arrival.
    ResourceRequest resources;
    Duration exec_duration;
    int failures_before_success;        ///< First N attempts fail.
    int max_retries;
};

std::vector<PlannedJob> generate_workload(const WorkloadConfig& cfg) {
    std::mt19937_64 rng(cfg.seed);
    std::exponential_distribution<double> interarrival(cfg.arrival_rate_hz);
    std::uniform_int_distribution<int> priority(cfg.priority_min, cfg.priority_max);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<std::int64_t> deadline_ns(cfg.deadline_min.count(),
                                                            cfg.deadline_max.count());
    std::uniform_int_distribution<std::uint32_t> cpu(cfg.cpu_min, cfg.cpu_max);
    std::uniform_int_distribution<std::uint32_t> mem(cfg.mem_min, cfg.mem_max);
    std::uniform_int_distribution<std::int64_t> exec_ns(cfg.exec_min.count(),
                                                        cfg.exec_max.count());

    std::vector<PlannedJob> plan;
    plan.reserve(static_cast<std::size_t>(cfg.jobs));
    Duration arrival{0};
    for (int i = 0; i < cfg.jobs; ++i) {
        arrival += std::chrono::duration_cast<Duration>(
            std::chrono::duration<double>(interarrival(rng)));

        PlannedJob job;
        job.arrival_offset = arrival;
        job.priority = priority(rng);
        if (unit(rng) < cfg.deadline_fraction) {
            job.deadline_offset = Duration{deadline_ns(rng)};
        }
        job.resources = {.cpu_units = cpu(rng), .memory_mb = mem(rng)};
        job.exec_duration = Duration{exec_ns(rng)};
        job.max_retries = cfg.max_retries;
        job.failures_before_success = 0;
        while (job.failures_before_success <= cfg.max_retries &&
               unit(rng) < cfg.failure_prob) {
            ++job.failures_before_success;
        }
        plan.push_back(job);
    }
    return plan;
}

/// ExecutionBackend that "runs" a job by scheduling its completion at
/// now + duration on the event heap. Reports started immediately --
/// simulated workers begin instantly.
class SimBackend final : public ExecutionBackend {
public:
    struct Completion {
        TimePoint at;
        JobId job;
        WorkerId worker;
        bool success;
        bool operator>(const Completion& o) const noexcept {
            if (at != o.at) {
                return at > o.at;
            }
            return job.value() > o.job.value();
        }
    };

    SimBackend(SchedulerClient& scheduler, Clock& clock,
               const std::vector<PlannedJob>& plan)
        : scheduler_(scheduler), clock_(clock), plan_(plan) {}

    void dispatch(JobAssignment assignment) override {
        scheduler_.report_started(assignment.worker_id, assignment.job_id);
        // Job ids are 1-based in submission order == plan order.
        const PlannedJob& planned = plan_[assignment.job_id.value() - 1];
        const bool success = assignment.job.attempt > planned.failures_before_success;
        completions_.push({clock_.now() + planned.exec_duration, assignment.job_id,
                           assignment.worker_id, success});
    }

    void start() override {}
    void stop() override {}

    [[nodiscard]] std::optional<TimePoint> next_completion() const {
        if (completions_.empty()) {
            return std::nullopt;
        }
        return completions_.top().at;
    }

    /// Deliver every completion due at or before `now`.
    void deliver_due(TimePoint now) {
        while (!completions_.empty() && completions_.top().at <= now) {
            const Completion c = completions_.top();
            completions_.pop();
            scheduler_.report_completion(
                c.worker, c.job,
                {.success = c.success, .error = c.success ? "" : "simulated failure"});
        }
    }

private:
    SchedulerClient& scheduler_;
    Clock& clock_;
    const std::vector<PlannedJob>& plan_;
    std::priority_queue<Completion, std::vector<Completion>, std::greater<>> completions_;
};

double percentile_of(std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double idx = p * static_cast<double>(sorted.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

}  // namespace

Simulation::Simulation(WorkloadConfig workload, ClusterConfig cluster)
    : workload_(std::move(workload)), cluster_(std::move(cluster)) {}

SimResult Simulation::run(const PolicyFactory& make_policy) const {
    const auto wall_start = std::chrono::steady_clock::now();

    const std::vector<PlannedJob> plan = generate_workload(workload_);

    SimulatedClock clock{TimePoint{} + std::chrono::hours(1)};
    const TimePoint t0 = clock.now();
    EventBus bus;
    JobStore store(clock, bus);
    WorkerRegistry registry(clock, bus);

    SchedulerConfig config;
    config.heartbeat_timeout = std::chrono::hours(24);  // No deaths in sim (yet).
    auto policy = make_policy();
    const std::string policy_name = policy->name();
    Scheduler scheduler(store, registry, bus, clock, std::move(policy),
                        RetryConfig{.base_backoff = 500ms, .jitter = 0.1,
                                    .seed = workload_.seed},
                        config);
    SimBackend backend(scheduler, clock, plan);
    scheduler.attach_backend(backend);
    for (std::size_t i = 0; i < cluster_.workers.size(); ++i) {
        registry.register_worker("sim-worker-" + std::to_string(i + 1),
                                 cluster_.workers[i]);
    }

    // ---- The discrete-event loop -----------------------------------------
    std::size_t next_arrival = 0;
    while (true) {
        // Submit every arrival due at the current instant.
        while (next_arrival < plan.size() &&
               t0 + plan[next_arrival].arrival_offset <= clock.now()) {
            const PlannedJob& p = plan[next_arrival];
            JobSpecBuilder builder;
            builder.name("sim-job-" + std::to_string(next_arrival + 1))
                .priority(p.priority)
                .max_retries(p.max_retries)
                .resources(p.resources);
            if (p.deadline_offset) {
                builder.deadline(clock.now() + *p.deadline_offset);
            }
            scheduler.submit(builder.build());
            ++next_arrival;
        }

        backend.deliver_due(clock.now());
        scheduler.run_once(clock.now());

        if (scheduler.live_jobs() == 0 && next_arrival == plan.size()) {
            break;  // Everything terminal, nothing left to arrive.
        }

        // Leap to the next event: arrival, completion, or retry ripening.
        std::optional<TimePoint> next;
        const auto consider = [&next](std::optional<TimePoint> t) {
            if (t && (!next || *t < *next)) {
                next = t;
            }
        };
        if (next_arrival < plan.size()) {
            consider(t0 + plan[next_arrival].arrival_offset);
        }
        consider(backend.next_completion());
        for (const Job& job : store.snapshot_in_state(JobState::RetryWait)) {
            consider(job.next_eligible_time);  // Retry ripenings.
        }
        if (!next) {
            break;  // Defensive: nothing can ever happen again.
        }
        // Rescore ticks between events keep aging honest.
        clock.advance_to(std::max(*next, clock.now() + std::chrono::milliseconds(1)));
    }

    // ---- Statistics, from the authoritative job histories ----------------
    SimResult r;
    r.policy = policy_name;
    r.jobs = workload_.jobs;

    std::vector<double> waits_ms;
    double wait_sum = 0, turnaround_sum = 0, turnaround_sq_sum = 0;
    int turnaround_n = 0;
    TimePoint last_event = t0;

    for (const Job& job : store.snapshot()) {
        std::optional<TimePoint> first_dispatch;
        TimePoint terminal = job.submit_time;
        for (const StateChange& hop : job.history) {
            if (hop.to == JobState::Dispatched && !first_dispatch) {
                first_dispatch = hop.at;
            }
            terminal = std::max(terminal, hop.at);
        }
        last_event = std::max(last_event, terminal);

        if (first_dispatch) {
            const double w = to_ms(*first_dispatch - job.submit_time);
            waits_ms.push_back(w);
            wait_sum += w;
        }
        if (job.state == JobState::Completed) {
            ++r.completed;
            const double t = to_ms(terminal - job.submit_time);
            turnaround_sum += t;
            turnaround_sq_sum += t * t;
            ++turnaround_n;
        } else if (job.state == JobState::Failed) {
            ++r.failed;
        }
        if (job.spec.deadline) {
            ++r.deadline_jobs;
            const bool met = job.state == JobState::Completed &&
                             terminal <= *job.spec.deadline;
            if (!met) {
                ++r.deadline_missed;
            }
        }
    }

    std::sort(waits_ms.begin(), waits_ms.end());
    r.wait_p50_ms = percentile_of(waits_ms, 0.50);
    r.wait_p95_ms = percentile_of(waits_ms, 0.95);
    r.wait_p99_ms = percentile_of(waits_ms, 0.99);
    const double n = static_cast<double>(waits_ms.size());
    r.wait_mean_ms = n > 0 ? wait_sum / n : 0;
    r.turnaround_mean_ms = r.completed > 0 ? turnaround_sum / r.completed : 0;
    r.deadline_miss_rate =
        r.deadline_jobs > 0
            ? static_cast<double>(r.deadline_missed) / r.deadline_jobs
            : 0;
    // Jain's index (Σx)²/(n·Σx²) over completed-job turnarounds. Waits are
    // the wrong base: under light load most waits are exactly 0 and the
    // index degenerates. Turnaround is always positive and captures the
    // same starvation signal (a starved job's turnaround balloons).
    r.jain_fairness = (turnaround_n > 0 && turnaround_sq_sum > 0)
                          ? (turnaround_sum * turnaround_sum) /
                                (turnaround_n * turnaround_sq_sum)
                          : 1.0;

    r.sim_duration_sec = std::chrono::duration<double>(last_event - t0).count();
    r.throughput_jobs_per_sec =
        r.sim_duration_sec > 0 ? r.completed / r.sim_duration_sec : 0;
    r.wall_time_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - wall_start)
                         .count();
    return r;
}

std::vector<SimResult> Simulation::compare() const {
    std::vector<SimResult> out;
    out.push_back(run([] { return std::make_unique<FifoPolicy>(); }));
    out.push_back(run([] { return std::make_unique<PriorityPolicy>(); }));
    out.push_back(run([] { return std::make_unique<EdfPolicy>(); }));
    out.push_back(run([] { return std::make_unique<CompositePolicy>(); }));
    return out;
}

std::string format_comparison(const std::vector<SimResult>& results) {
    std::string out;
    char line[256];
    std::snprintf(line, sizeof(line), "%-10s %9s %7s %10s %10s %10s %11s %9s %9s\n",
                  "policy", "completed", "failed", "wait_p50", "wait_p99",
                  "wait_mean", "ddl_miss", "jain", "thru/s");
    out += line;
    out += std::string(92, '-') + "\n";
    for (const SimResult& r : results) {
        std::snprintf(line, sizeof(line),
                      "%-10s %9d %7d %8.0fms %8.0fms %8.0fms %10.1f%% %9.3f %9.1f\n",
                      r.policy.c_str(), r.completed, r.failed, r.wait_p50_ms,
                      r.wait_p99_ms, r.wait_mean_ms, r.deadline_miss_rate * 100.0,
                      r.jain_fairness, r.throughput_jobs_per_sec);
        out += line;
    }
    return out;
}

std::string result_csv_header() {
    return "policy,jobs,completed,failed,sim_duration_sec,throughput_jobs_per_sec,"
           "wait_p50_ms,wait_p95_ms,wait_p99_ms,wait_mean_ms,turnaround_mean_ms,"
           "deadline_jobs,deadline_missed,deadline_miss_rate,jain_fairness,wall_time_ms\n";
}

}  // namespace chronos::sim
