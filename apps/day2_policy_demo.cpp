// Day 2 demo: the scheduling brain.
// One mixed workload, ranked by all four policies side by side -- a tiny
// preview of the Phase-4 simulator's policy comparisons. Also shows aging
// via rescore() and a full retry-backoff cycle, all on simulated time.

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "chronos/core/clock.h"
#include "chronos/scheduling/policies.h"
#include "chronos/scheduling/ready_set.h"
#include "chronos/scheduling/retry_manager.h"

using namespace chronos;
using namespace std::chrono_literals;

namespace {

Job make_job(std::uint64_t id, std::string name, int priority,
             std::optional<Duration> deadline_in, TimePoint submit, TimePoint t0) {
    Job job;
    job.id = JobId{id};
    JobSpecBuilder builder;
    builder.name(std::move(name)).priority(priority);
    if (deadline_in) {
        builder.deadline(t0 + *deadline_in);
    }
    job.spec = builder.build();
    job.submit_time = submit;
    return job;
}

void print_ranking(const char* title, const SchedulingPolicy& policy,
                   const std::vector<Job>& jobs, TimePoint now) {
    ReadySet set(policy);
    for (const Job& job : jobs) {
        set.add(job, now);
    }
    std::cout << "  " << std::left << std::setw(11) << title << " ";
    bool first = true;
    for (const auto& [score, id] : set.entries()) {
        for (const Job& job : jobs) {
            if (job.id == id) {
                std::cout << (first ? "" : " > ") << job.spec.name;
                first = false;
            }
        }
    }
    std::cout << "\n";
}

}  // namespace

int main() {
    SimulatedClock clock{TimePoint{} + 1000s};
    const TimePoint t0 = clock.now();

    // A workload where every policy disagrees:
    //   backup:  submitted 5 min ago, low priority, no deadline
    //   alert:   just arrived, top priority, no deadline
    //   report:  2 min old, mid priority, deadline in 10s (urgent!)
    //   batch:   3 min old, mid priority, deadline in 10 min
    const std::vector<Job> jobs = {
        make_job(1, "backup", 1, std::nullopt, t0 - 300s, t0),
        make_job(2, "alert", 9, std::nullopt, t0, t0),
        make_job(3, "report", 5, 10s, t0 - 120s, t0),
        make_job(4, "batch", 5, 600s, t0 - 180s, t0),
    };

    std::cout << "One workload, four policies -- who runs first?\n";
    print_ranking("FIFO", FifoPolicy{}, jobs, t0);
    print_ranking("Priority", PriorityPolicy{}, jobs, t0);
    print_ranking("EDF", EdfPolicy{}, jobs, t0);
    print_ranking("Composite", CompositePolicy{}, jobs, t0);

    // --- Aging: the starving job climbs the ranking on rescore ----------
    std::cout << "\nAging under Composite (rescore as simulated time passes):\n";
    CompositePolicy composite;
    ReadySet set(composite);
    for (const Job& job : jobs) {
        set.add(job, t0);
    }
    for (int minutes : {0, 10, 30}) {
        clock.advance_to(t0 + std::chrono::minutes(minutes));
        set.rescore(jobs, clock.now());
        std::cout << "  t+" << std::setw(2) << minutes << "min  ";
        bool first = true;
        for (const auto& [score, id] : set.entries()) {
            for (const Job& job : jobs) {
                if (job.id == id) {
                    std::cout << (first ? "" : " > ") << job.spec.name;
                    first = false;
                }
            }
        }
        std::cout << "\n";
    }

    // --- Retry backoff on simulated time ---------------------------------
    std::cout << "\nRetry backoff (base 1s, x2, jitter off):\n";
    RetryManager retries(RetryConfig{.jitter = 0.0});
    Job flaky = make_job(9, "flaky-etl", 3, std::nullopt, clock.now(), clock.now());
    for (flaky.attempt = 1; flaky.attempt <= 4; ++flaky.attempt) {
        flaky.spec = JobSpecBuilder{}.name("flaky-etl").max_retries(3).build();
        const auto d = retries.decide(flaky, clock.now());
        if (d.action == RetryAction::Retry) {
            std::cout << "  attempt " << flaky.attempt << " failed -> retry in "
                      << std::chrono::duration_cast<std::chrono::seconds>(d.delay).count()
                      << "s\n";
        } else {
            std::cout << "  attempt " << flaky.attempt
                      << " failed -> retries exhausted, dead-letter\n";
        }
    }
    return 0;
}
