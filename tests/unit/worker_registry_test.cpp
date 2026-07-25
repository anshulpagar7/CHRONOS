#include "chronos/execution/worker_registry.h"

#include <gtest/gtest.h>

#include <chrono>

namespace chronos {
namespace {

using namespace std::chrono_literals;

class WorkerRegistryTest : public ::testing::Test {
protected:
    SimulatedClock clock_{TimePoint{} + 1000s};
    EventBus bus_;
    WorkerRegistry registry_{clock_, bus_};
};

TEST_F(WorkerRegistryTest, RegisterCreatesAliveWorkerWithFullCapacity) {
    const WorkerId id = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 1024});

    const auto info = registry_.get(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->alive);
    EXPECT_EQ(info->name, "w1");
    EXPECT_EQ(info->available.cpu_units, 4u);
    EXPECT_EQ(info->available.memory_mb, 1024u);
    EXPECT_EQ(info->last_heartbeat, clock_.now());
    EXPECT_EQ(registry_.alive_count(), 1u);
}

TEST_F(WorkerRegistryTest, RegisterEmitsWorkerRegisteredEvent) {
    Event seen{};
    bus_.subscribe([&seen](const Event& e) { seen = e; });

    const WorkerId id = registry_.register_worker("w1", {.cpu_units = 1, .memory_mb = 64});

    EXPECT_EQ(seen.type, EventType::WorkerRegistered);
    EXPECT_EQ(seen.worker_id, id);
    EXPECT_EQ(seen.detail, "w1");
}

TEST_F(WorkerRegistryTest, AllocateReservesAndReleaseRestores) {
    const WorkerId id = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 1024});
    const ResourceRequest req{.cpu_units = 3, .memory_mb = 512};

    EXPECT_TRUE(registry_.try_allocate(id, req, JobId{1}));
    auto info = registry_.get(id);
    EXPECT_EQ(info->available.cpu_units, 1u);
    EXPECT_EQ(info->available.memory_mb, 512u);
    EXPECT_TRUE(info->running_jobs.contains(JobId{1}));

    EXPECT_TRUE(registry_.release(id, req, JobId{1}));
    info = registry_.get(id);
    EXPECT_EQ(info->available.cpu_units, 4u);
    EXPECT_EQ(info->available.memory_mb, 1024u);
    EXPECT_TRUE(info->running_jobs.empty());
}

TEST_F(WorkerRegistryTest, AllocateFailsWithoutCapacity) {
    const WorkerId id = registry_.register_worker("w1", {.cpu_units = 2, .memory_mb = 256});

    EXPECT_FALSE(registry_.try_allocate(id, {.cpu_units = 3, .memory_mb = 64}, JobId{1}));
    EXPECT_FALSE(registry_.try_allocate(id, {.cpu_units = 1, .memory_mb = 512}, JobId{1}));

    // Nothing was charged by the failed attempts.
    const auto info = registry_.get(id);
    EXPECT_EQ(info->available.cpu_units, 2u);
    EXPECT_EQ(info->available.memory_mb, 256u);
}

TEST_F(WorkerRegistryTest, AllocateFailsForUnknownWorkerOrDuplicateJob) {
    const WorkerId id = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 512});

    EXPECT_FALSE(registry_.try_allocate(WorkerId{99}, {.cpu_units = 1, .memory_mb = 64},
                                        JobId{1}));

    EXPECT_TRUE(registry_.try_allocate(id, {.cpu_units = 1, .memory_mb = 64}, JobId{1}));
    EXPECT_FALSE(registry_.try_allocate(id, {.cpu_units = 1, .memory_mb = 64}, JobId{1}));
}

TEST_F(WorkerRegistryTest, DoubleReleaseIsRefused) {
    const WorkerId id = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 512});
    const ResourceRequest req{.cpu_units = 2, .memory_mb = 128};

    ASSERT_TRUE(registry_.try_allocate(id, req, JobId{1}));
    EXPECT_TRUE(registry_.release(id, req, JobId{1}));
    EXPECT_FALSE(registry_.release(id, req, JobId{1}));  // Second must refuse...

    const auto info = registry_.get(id);
    EXPECT_EQ(info->available.cpu_units, 4u);  // ...and never over-credit.
    EXPECT_EQ(info->available.memory_mb, 512u);
}

TEST_F(WorkerRegistryTest, FindFitPrefersMostFreeCpuThenLowestId) {
    const WorkerId w1 = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 512});
    const WorkerId w2 = registry_.register_worker("w2", {.cpu_units = 8, .memory_mb = 512});
    const WorkerId w3 = registry_.register_worker("w3", {.cpu_units = 8, .memory_mb = 512});

    // w2 and w3 tie on free CPU (8): lowest id wins.
    EXPECT_EQ(*registry_.find_fit({.cpu_units = 2, .memory_mb = 64}), w2);

    // Load w2 down; now w3 has the most free CPU.
    ASSERT_TRUE(registry_.try_allocate(w2, {.cpu_units = 6, .memory_mb = 64}, JobId{1}));
    EXPECT_EQ(*registry_.find_fit({.cpu_units = 2, .memory_mb = 64}), w3);

    // Excluding w3 (reservation) falls back to w1.
    ASSERT_TRUE(registry_.try_allocate(w3, {.cpu_units = 6, .memory_mb = 64}, JobId{2}));
    EXPECT_EQ(*registry_.find_fit({.cpu_units = 3, .memory_mb = 64}, w3), w1);

    // Nothing fits a 16-cpu request.
    EXPECT_FALSE(registry_.find_fit({.cpu_units = 16, .memory_mb = 64}).has_value());
}

TEST_F(WorkerRegistryTest, ReservationTargetUsesTotalCapacityNotFree) {
    registry_.register_worker("small", {.cpu_units = 2, .memory_mb = 512});
    const WorkerId big = registry_.register_worker("big", {.cpu_units = 8, .memory_mb = 512});

    // Fill the big worker completely: no *free* capacity anywhere for 8 cpu.
    ASSERT_TRUE(registry_.try_allocate(big, {.cpu_units = 8, .memory_mb = 256}, JobId{1}));
    EXPECT_FALSE(registry_.find_fit({.cpu_units = 8, .memory_mb = 64}).has_value());

    // But its TOTAL could host the job once drained: it is the target.
    EXPECT_EQ(*registry_.find_reservation_target({.cpu_units = 8, .memory_mb = 64}), big);

    // A request no worker could ever host has no target.
    EXPECT_FALSE(registry_.find_reservation_target({.cpu_units = 32, .memory_mb = 64})
                     .has_value());
}

TEST_F(WorkerRegistryTest, CollectDeadMarksSilentWorkersAndReturnsOrphans) {
    const WorkerId w1 = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 512});
    const WorkerId w2 = registry_.register_worker("w2", {.cpu_units = 4, .memory_mb = 512});
    ASSERT_TRUE(registry_.try_allocate(w1, {.cpu_units = 2, .memory_mb = 128}, JobId{7}));

    clock_.advance(2s);
    registry_.heartbeat(w2);  // Only w2 stays fresh.
    clock_.advance(2s);       // w1 last beat: 4s ago; w2: 2s ago.

    Event dead_event{};
    bus_.subscribe([&dead_event](const Event& e) { dead_event = e; });

    const auto dead = registry_.collect_dead(/*timeout=*/3s);

    ASSERT_EQ(dead.size(), 1u);
    EXPECT_EQ(dead[0].id, w1);
    ASSERT_EQ(dead[0].orphaned_jobs.size(), 1u);
    EXPECT_EQ(dead[0].orphaned_jobs[0], JobId{7});
    EXPECT_EQ(dead_event.type, EventType::WorkerMarkedDead);
    EXPECT_EQ(dead_event.worker_id, w1);

    EXPECT_EQ(registry_.alive_count(), 1u);
    EXPECT_FALSE(registry_.get(w1)->alive);
    EXPECT_EQ(registry_.get(w1)->available.cpu_units, 0u);
}

TEST_F(WorkerRegistryTest, DeadWorkersRejectEverything) {
    const WorkerId id = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 512});
    const ResourceRequest req{.cpu_units = 1, .memory_mb = 64};
    ASSERT_TRUE(registry_.try_allocate(id, req, JobId{1}));

    clock_.advance(10s);
    ASSERT_EQ(registry_.collect_dead(3s).size(), 1u);

    EXPECT_FALSE(registry_.heartbeat(id));            // Death is permanent.
    EXPECT_FALSE(registry_.try_allocate(id, req, JobId{2}));
    EXPECT_FALSE(registry_.release(id, req, JobId{1}));  // Sweep freed it.
    EXPECT_FALSE(registry_.find_fit(req).has_value());

    // A second sweep reports nothing new.
    clock_.advance(10s);
    EXPECT_TRUE(registry_.collect_dead(3s).empty());
}

TEST_F(WorkerRegistryTest, HeartbeatKeepsWorkerAlive) {
    const WorkerId id = registry_.register_worker("w1", {.cpu_units = 1, .memory_mb = 64});
    for (int i = 0; i < 5; ++i) {
        clock_.advance(2s);
        EXPECT_TRUE(registry_.heartbeat(id));
    }
    EXPECT_TRUE(registry_.collect_dead(3s).empty());
    EXPECT_EQ(registry_.alive_count(), 1u);
}

}  // namespace
}  // namespace chronos
