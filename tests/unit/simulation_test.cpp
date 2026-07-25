#include "chronos/sim/simulation.h"

#include <gtest/gtest.h>

#include <memory>

#include "chronos/scheduling/policies.h"

namespace chronos::sim {
namespace {

WorkloadConfig small_workload() {
    WorkloadConfig w;
    w.jobs = 300;
    w.seed = 42;
    w.arrival_rate_hz = 12.0;
    w.failure_prob = 0.1;
    return w;
}

TEST(Simulation, SameSeedIsBitIdentical) {
    const Simulation sim(small_workload(), ClusterConfig{});
    const auto factory = [] { return std::make_unique<CompositePolicy>(); };

    const SimResult a = sim.run(factory);
    const SimResult b = sim.run(factory);

    EXPECT_EQ(a.completed, b.completed);
    EXPECT_EQ(a.failed, b.failed);
    EXPECT_EQ(a.deadline_missed, b.deadline_missed);
    EXPECT_DOUBLE_EQ(a.wait_p99_ms, b.wait_p99_ms);
    EXPECT_DOUBLE_EQ(a.wait_mean_ms, b.wait_mean_ms);
    EXPECT_DOUBLE_EQ(a.jain_fairness, b.jain_fairness);
    EXPECT_DOUBLE_EQ(a.sim_duration_sec, b.sim_duration_sec);
}

TEST(Simulation, DifferentSeedsProduceDifferentWorkloads) {
    WorkloadConfig w = small_workload();
    const auto factory = [] { return std::make_unique<CompositePolicy>(); };

    const SimResult a = Simulation(w, ClusterConfig{}).run(factory);
    w.seed = 1337;
    const SimResult b = Simulation(w, ClusterConfig{}).run(factory);

    EXPECT_NE(a.sim_duration_sec, b.sim_duration_sec);
}

TEST(Simulation, EveryJobReachesExactlyOneTerminalState) {
    const Simulation sim(small_workload(), ClusterConfig{});
    for (const SimResult& r : sim.compare()) {
        EXPECT_EQ(r.completed + r.failed, r.jobs) << r.policy;
        EXPECT_GT(r.completed, 0) << r.policy;
        EXPECT_GT(r.throughput_jobs_per_sec, 0.0) << r.policy;
        EXPECT_GE(r.wait_p99_ms, r.wait_p50_ms) << r.policy;
        EXPECT_GT(r.jain_fairness, 0.0) << r.policy;
        EXPECT_LE(r.jain_fairness, 1.0) << r.policy;
    }
}

TEST(Simulation, EdfMissesFewerDeadlinesThanFifoOnThisWorkload) {
    // Deterministic per seed, so this is a stable regression test of the
    // one property EDF exists to provide. The workload must be *feasible
    // but contended*: under heavy overload every policy misses whatever
    // cannot physically be done and EDF's edge washes out (the classic
    // EDF-under-overload domino effect), so keep utilization below 1.
    WorkloadConfig w = small_workload();
    w.deadline_fraction = 0.9;
    w.arrival_rate_hz = 10.0;
    w.exec_min = std::chrono::milliseconds(50);
    w.exec_max = std::chrono::milliseconds(700);
    w.deadline_min = std::chrono::milliseconds(800);
    w.deadline_max = std::chrono::seconds(8);
    w.failure_prob = 0.0;  // Isolate the scheduling effect.
    const Simulation sim(w, ClusterConfig{});

    const SimResult fifo = sim.run([] { return std::make_unique<FifoPolicy>(); });
    const SimResult edf = sim.run([] { return std::make_unique<EdfPolicy>(); });

    ASSERT_GT(fifo.deadline_jobs, 0);
    EXPECT_LE(edf.deadline_missed, fifo.deadline_missed);
}

TEST(Simulation, ComparisonTableRendersAllPolicies) {
    WorkloadConfig w = small_workload();
    w.jobs = 100;
    const auto results = Simulation(w, ClusterConfig{}).compare();
    const std::string table = format_comparison(results);
    for (const char* name : {"fifo", "priority", "edf", "composite"}) {
        EXPECT_NE(table.find(name), std::string::npos) << name;
    }
}

}  // namespace
}  // namespace chronos::sim
