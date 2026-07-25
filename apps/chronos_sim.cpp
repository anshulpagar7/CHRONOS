// chronos-sim: run synthetic workloads through the REAL scheduler on
// simulated time. Thousands of jobs, hours of simulated waiting -- in
// milliseconds of wall time, bit-identically reproducible per seed.
//
//   chronos-sim --compare                        # all 4 policies, one table
//   chronos-sim --policy=edf --jobs=10000        # one policy, full detail
//   chronos-sim --compare --seed=7 --csv         # + CSV rows for plotting

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "chronos/observability/log.h"
#include "chronos/scheduling/policies.h"
#include "chronos/sim/simulation.h"

using namespace chronos;
using namespace chronos::sim;

namespace {

bool parse_flag(std::string_view arg, std::string_view name, std::string& value) {
    if (arg.substr(0, name.size()) == name && arg.size() > name.size() &&
        arg[name.size()] == '=') {
        value = std::string(arg.substr(name.size() + 1));
        return true;
    }
    return false;
}

Simulation::PolicyFactory factory_for(const std::string& name) {
    if (name == "fifo") return [] { return std::make_unique<FifoPolicy>(); };
    if (name == "priority") return [] { return std::make_unique<PriorityPolicy>(); };
    if (name == "edf") return [] { return std::make_unique<EdfPolicy>(); };
    if (name == "composite") return [] { return std::make_unique<CompositePolicy>(); };
    std::fprintf(stderr, "unknown policy '%s' (fifo|priority|edf|composite)\n",
                 name.c_str());
    std::exit(2);
}

void print_csv_row(const SimResult& r) {
    std::printf("%s,%d,%d,%d,%.3f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%.4f,%.4f,%.2f\n",
                r.policy.c_str(), r.jobs, r.completed, r.failed, r.sim_duration_sec,
                r.throughput_jobs_per_sec, r.wait_p50_ms, r.wait_p95_ms, r.wait_p99_ms,
                r.wait_mean_ms, r.turnaround_mean_ms, r.deadline_jobs,
                r.deadline_missed, r.deadline_miss_rate, r.jain_fairness,
                r.wall_time_ms);
}

}  // namespace

int main(int argc, char** argv) {
    // The simulator is a measurement tool: keep the event stream quiet.
    chronos::log::set_level(chronos::log::Level::Warn);

    WorkloadConfig workload;
    ClusterConfig cluster;
    std::string policy_name = "composite";
    bool compare = false;
    bool csv = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::string value;
        if (arg == "--compare") {
            compare = true;
        } else if (arg == "--csv") {
            csv = true;
        } else if (parse_flag(arg, "--policy", value)) {
            policy_name = value;
        } else if (parse_flag(arg, "--jobs", value)) {
            workload.jobs = std::atoi(value.c_str());
        } else if (parse_flag(arg, "--seed", value)) {
            workload.seed = static_cast<std::uint64_t>(std::atoll(value.c_str()));
        } else if (parse_flag(arg, "--arrival-hz", value)) {
            workload.arrival_rate_hz = std::atof(value.c_str());
        } else if (parse_flag(arg, "--failure-prob", value)) {
            workload.failure_prob = std::atof(value.c_str());
        } else {
            std::fprintf(stderr,
                         "usage: chronos-sim [--compare] [--policy=NAME] [--jobs=N]\n"
                         "                   [--seed=S] [--arrival-hz=R]\n"
                         "                   [--failure-prob=P] [--csv]\n");
            return 2;
        }
    }

    const Simulation simulation(workload, cluster);

    std::printf("chronos-sim: %d jobs, seed %llu, %.0f jobs/s arrivals, "
                "%zu workers, %.0f%% failure prob\n\n",
                workload.jobs, static_cast<unsigned long long>(workload.seed),
                workload.arrival_rate_hz, cluster.workers.size(),
                workload.failure_prob * 100.0);

    std::vector<SimResult> results;
    if (compare) {
        results = simulation.compare();
    } else {
        results.push_back(simulation.run(factory_for(policy_name)));
    }

    std::printf("%s", format_comparison(results).c_str());
    double total_wall = 0;
    for (const auto& r : results) {
        total_wall += r.wall_time_ms;
    }
    std::printf("\n(%zu run%s, %.0f ms wall time total)\n", results.size(),
                results.size() == 1 ? "" : "s", total_wall);

    if (csv) {
        std::printf("\n%s", result_csv_header().c_str());
        for (const auto& r : results) {
            print_csv_row(r);
        }
    }
    return 0;
}
