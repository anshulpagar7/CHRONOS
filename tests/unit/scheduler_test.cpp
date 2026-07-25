#include "chronos/scheduling/scheduler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <vector>

#include "chronos/scheduling/policies.h"

namespace chronos {
namespace {

using namespace std::chrono_literals;

/// Records assignments; the test plays the role of the workers by calling
/// report_started / report_completion on the scheduler directly.
class MockBackend final : public ExecutionBackend {
public:
    void dispatch(JobAssignment assignment) override {
        assignments.push_back(std::move(assignment));
    }
    void start() override {}
    void stop() override {}

    std::vector<JobAssignment> assignments;
};

class SchedulerTest : public ::testing::Test {
protected:
    SchedulerTest() {
        SchedulerConfig config;
        config.heartbeat_timeout = 1h;  // Liveness off unless a test wants it.
        config.rescore_interval = 500ms;
        config.backfill_skip_threshold = 3;
        scheduler_ = std::make_unique<Scheduler>(
            store_, registry_, bus_, clock_, std::make_unique<PriorityPolicy>(),
            RetryConfig{.base_backoff = 2s, .jitter = 0.0}, config);
        scheduler_->attach_backend(backend_);
    }

    JobState state(JobId id) { return store_.get(id)->state; }

    /// Convenience: run one iteration at the current simulated time.
    void tick() { scheduler_->run_once(clock_.now()); }

    SimulatedClock clock_{TimePoint{} + 1000s};
    EventBus bus_;
    JobStore store_{clock_, bus_};
    WorkerRegistry registry_{clock_, bus_};
    MockBackend backend_;
    std::unique_ptr<Scheduler> scheduler_;
};

// ---------------------------------------------------------------------------
// Dispatch basics
// ---------------------------------------------------------------------------

TEST_F(SchedulerTest, SubmitDispatchesToFittingWorker) {
    const WorkerId w = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 512});

    const JobId id = scheduler_->submit(
        JobSpecBuilder{}.resources({.cpu_units = 2, .memory_mb = 128}).build());
    EXPECT_EQ(state(id), JobState::Queued);

    tick();

    EXPECT_EQ(state(id), JobState::Dispatched);
    ASSERT_EQ(backend_.assignments.size(), 1u);
    EXPECT_EQ(backend_.assignments[0].job_id, id);
    EXPECT_EQ(backend_.assignments[0].worker_id, w);
    // Resources charged.
    EXPECT_EQ(registry_.get(w)->available.cpu_units, 2u);
}

TEST_F(SchedulerTest, HigherPriorityDispatchesFirstUnderContention) {
    // One 1-cpu worker: only one job can run at a time.
    registry_.register_worker("w1", {.cpu_units = 1, .memory_mb = 512});

    const JobId low = scheduler_->submit(JobSpecBuilder{}.priority(1).build());
    const JobId high = scheduler_->submit(JobSpecBuilder{}.priority(9).build());

    tick();

    EXPECT_EQ(state(high), JobState::Dispatched);
    EXPECT_EQ(state(low), JobState::Queued);  // Waiting for capacity.
}

TEST_F(SchedulerTest, CompletionReleasesResourcesAndUnblocksNextJob) {
    const WorkerId w = registry_.register_worker("w1", {.cpu_units = 1, .memory_mb = 512});
    const JobId a = scheduler_->submit(JobSpecBuilder{}.priority(9).build());
    const JobId b = scheduler_->submit(JobSpecBuilder{}.priority(1).build());
    tick();
    ASSERT_EQ(state(a), JobState::Dispatched);

    scheduler_->report_started(w, a);
    tick();
    EXPECT_EQ(state(a), JobState::Running);

    scheduler_->report_completion(w, a, {.success = true, .error = {}});
    tick();

    EXPECT_EQ(state(a), JobState::Completed);
    EXPECT_EQ(state(b), JobState::Dispatched);  // Freed capacity flowed to b.
    EXPECT_EQ(scheduler_->live_jobs(), 1u);
}

TEST_F(SchedulerTest, NoFitLeavesJobQueued) {
    registry_.register_worker("w1", {.cpu_units = 2, .memory_mb = 128});
    const JobId id = scheduler_->submit(
        JobSpecBuilder{}.resources({.cpu_units = 8, .memory_mb = 64}).build());

    tick();
    EXPECT_EQ(state(id), JobState::Queued);
    EXPECT_TRUE(backend_.assignments.empty());
}

// ---------------------------------------------------------------------------
// Retry cycle
// ---------------------------------------------------------------------------

TEST_F(SchedulerTest, FailureBacksOffThenRetriesOnSchedule) {
    const WorkerId w = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 512});
    const JobId id = scheduler_->submit(JobSpecBuilder{}.max_retries(2).build());
    tick();
    scheduler_->report_started(w, id);
    tick();

    scheduler_->report_completion(w, id, {.success = false, .error = "boom"});
    tick();
    EXPECT_EQ(state(id), JobState::RetryWait);
    EXPECT_EQ(store_.get(id)->next_eligible_time, clock_.now() + 2s);
    // Resources already back.
    EXPECT_EQ(registry_.get(w)->available.cpu_units, 4u);

    clock_.advance(1999ms);
    tick();
    EXPECT_EQ(state(id), JobState::RetryWait);  // 1ms early: still parked.

    clock_.advance(1ms);
    tick();
    EXPECT_EQ(state(id), JobState::Dispatched);  // Ripened, requeued, redispatched.
    EXPECT_EQ(backend_.assignments.size(), 2u);
}

TEST_F(SchedulerTest, RetryBudgetExhaustionDeadLetters) {
    const WorkerId w = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 512});
    const JobId id = scheduler_->submit(JobSpecBuilder{}.max_retries(1).build());

    for (int attempt = 1; attempt <= 2; ++attempt) {
        tick();
        ASSERT_EQ(state(id), JobState::Dispatched) << "attempt " << attempt;
        scheduler_->report_started(w, id);
        tick();
        scheduler_->report_completion(w, id, {.success = false, .error = {}});
        tick();               // Process the failure at the current time...
        clock_.advance(10s);  // ...then sail past any backoff.
    }
    tick();

    EXPECT_EQ(state(id), JobState::Failed);
    EXPECT_EQ(store_.get(id)->attempt, 2);  // max_retries=1 -> 2 total runs.
    EXPECT_EQ(scheduler_->live_jobs(), 0u);
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

TEST_F(SchedulerTest, CancelQueuedJobBeforeAndAfterItReachesReadySet) {
    // No workers: everything stays queued.
    const JobId a = scheduler_->submit(JobSpecBuilder{}.build());
    tick();  // a is now in the ready set.
    const JobId b = scheduler_->submit(JobSpecBuilder{}.build());

    EXPECT_TRUE(scheduler_->cancel(a));
    EXPECT_TRUE(scheduler_->cancel(b));  // Cancelled before its QueuedCmd ran.
    tick();

    EXPECT_EQ(state(a), JobState::Cancelled);
    EXPECT_EQ(state(b), JobState::Cancelled);
    EXPECT_EQ(scheduler_->live_jobs(), 0u);

    registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 512});
    tick();
    EXPECT_TRUE(backend_.assignments.empty());  // Nothing left to dispatch.
}

TEST_F(SchedulerTest, CancelRefusedForRunningAndTerminalJobs) {
    const WorkerId w = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 512});
    const JobId id = scheduler_->submit(JobSpecBuilder{}.build());
    tick();
    scheduler_->report_started(w, id);
    tick();

    EXPECT_FALSE(scheduler_->cancel(id));  // Running.
    scheduler_->report_completion(w, id, {.success = true, .error = {}});
    tick();
    EXPECT_FALSE(scheduler_->cancel(id));  // Completed.
    EXPECT_EQ(state(id), JobState::Completed);
}

// ---------------------------------------------------------------------------
// Dead-worker rescue
// ---------------------------------------------------------------------------

TEST_F(SchedulerTest, DeadWorkerJobIsRescuedAndRedispatched) {
    SchedulerConfig config;
    config.heartbeat_timeout = 3s;
    Scheduler scheduler(store_, registry_, bus_, clock_,
                        std::make_unique<PriorityPolicy>(),
                        RetryConfig{.jitter = 0.0}, config);
    MockBackend backend;
    scheduler.attach_backend(backend);

    const WorkerId w1 = registry_.register_worker("w1", {.cpu_units = 4, .memory_mb = 512});
    const WorkerId w2 = registry_.register_worker("w2", {.cpu_units = 4, .memory_mb = 512});

    const JobId id = scheduler.submit(JobSpecBuilder{}.build());
    scheduler.run_once(clock_.now());
    const WorkerId assigned = backend.assignments.at(0).worker_id;
    scheduler.report_started(assigned, id);
    scheduler.run_once(clock_.now());
    ASSERT_EQ(store_.get(id)->state, JobState::Running);

    // The other worker keeps beating; the assigned one goes silent.
    const WorkerId survivor = (assigned == w1) ? w2 : w1;
    clock_.advance(2s);
    scheduler.report_heartbeat(survivor);
    clock_.advance(2s);  // assigned: 4s silent > 3s timeout.
    scheduler.run_once(clock_.now());

    // Rescued: Running -> Queued -> redispatched to the survivor.
    ASSERT_EQ(backend.assignments.size(), 2u);
    EXPECT_EQ(backend.assignments[1].worker_id, survivor);
    EXPECT_EQ(store_.get(id)->state, JobState::Dispatched);
    EXPECT_EQ(store_.get(id)->attempt, 1);  // Rescue is not a retry.
    EXPECT_FALSE(registry_.get(assigned)->alive);

    // The dead worker's completion report arrives late: must be ignored.
    scheduler.report_completion(assigned, id, {.success = true, .error = {}});
    scheduler.run_once(clock_.now());
    EXPECT_EQ(store_.get(id)->state, JobState::Dispatched);  // Unmoved.
}

// ---------------------------------------------------------------------------
// Backfill reservation
// ---------------------------------------------------------------------------

TEST_F(SchedulerTest, StarvedBigJobEarnsReservationAndEventuallyRuns) {
    const WorkerId big = registry_.register_worker("big", {.cpu_units = 8, .memory_mb = 1024});
    const WorkerId small = registry_.register_worker("small", {.cpu_units = 2, .memory_mb = 1024});

    // Fill the big worker with a long-running job (2 cpu left).
    const JobId filler = scheduler_->submit(
        JobSpecBuilder{}.name("filler").priority(1)
            .resources({.cpu_units = 6, .memory_mb = 64}).build());
    tick();
    ASSERT_EQ(backend_.assignments.at(0).worker_id, big);
    scheduler_->report_started(big, filler);

    // The whale: needs 8 cpu -- fits nowhere while filler runs.
    const JobId whale = scheduler_->submit(
        JobSpecBuilder{}.name("whale").priority(9)
            .resources({.cpu_units = 8, .memory_mb = 256}).build());
    tick();
    EXPECT_EQ(state(whale), JobState::Queued);  // Waiting; no skips yet.

    // A stream of small jobs keeps dispatching PAST the whale -- that is
    // starvation, and each jump counts a skip (threshold: 3).
    for (int i = 0; i < 3; ++i) {
        const JobId minnow = scheduler_->submit(
            JobSpecBuilder{}.name("minnow").priority(5)
                .resources({.cpu_units = 2, .memory_mb = 64}).build());
        tick();
        ASSERT_EQ(state(minnow), JobState::Dispatched);
        const WorkerId where = backend_.assignments.back().worker_id;
        scheduler_->report_started(where, minnow);
        scheduler_->report_completion(where, minnow, {.success = true, .error = {}});
        tick();
        EXPECT_EQ(state(whale), JobState::Queued);
    }

    // Threshold reached: the biggest capable worker is now reserved.
    const auto reservation = scheduler_->active_reservation();
    ASSERT_TRUE(reservation.has_value());
    EXPECT_EQ(reservation->first, whale);
    EXPECT_EQ(reservation->second, big);

    // The next minnow must NOT leak onto the reserved worker, even though
    // the big worker has 2 free cpu; it routes to `small`.
    const JobId minnow = scheduler_->submit(
        JobSpecBuilder{}.name("late-minnow").priority(5)
            .resources({.cpu_units = 2, .memory_mb = 64}).build());
    tick();
    EXPECT_EQ(state(minnow), JobState::Dispatched);
    EXPECT_EQ(backend_.assignments.back().worker_id, small);
    EXPECT_EQ(backend_.assignments.back().job_id, minnow);

    // Filler finishes; the big worker drains; the whale finally runs there.
    scheduler_->report_completion(big, filler, {.success = true, .error = {}});
    tick();

    EXPECT_EQ(state(whale), JobState::Dispatched);
    EXPECT_EQ(backend_.assignments.back().job_id, whale);
    EXPECT_EQ(backend_.assignments.back().worker_id, big);
    EXPECT_FALSE(scheduler_->active_reservation().has_value());  // Cleared.
}

TEST_F(SchedulerTest, SaturationAloneNeverTriggersReservation) {
    // One worker, one queued job that cannot fit while the worker is busy,
    // and NO smaller jobs jumping it: skips must stay 0 and no reservation
    // may appear, no matter how many rounds pass.
    const WorkerId big = registry_.register_worker("big", {.cpu_units = 8, .memory_mb = 1024});
    const JobId filler = scheduler_->submit(
        JobSpecBuilder{}.resources({.cpu_units = 6, .memory_mb = 64}).build());
    tick();
    scheduler_->report_started(big, filler);

    const JobId whale = scheduler_->submit(
        JobSpecBuilder{}.priority(9).resources({.cpu_units = 8, .memory_mb = 64}).build());
    for (int i = 0; i < 10; ++i) {
        tick();
    }

    EXPECT_EQ(state(whale), JobState::Queued);
    EXPECT_FALSE(scheduler_->active_reservation().has_value());
}

TEST_F(SchedulerTest, ReservationClearsIfReservedJobIsCancelled) {
    const WorkerId big = registry_.register_worker("big", {.cpu_units = 8, .memory_mb = 1024});
    const JobId filler = scheduler_->submit(
        JobSpecBuilder{}.resources({.cpu_units = 6, .memory_mb = 64}).build());
    tick();
    scheduler_->report_started(big, filler);

    const JobId whale = scheduler_->submit(
        JobSpecBuilder{}.priority(9).resources({.cpu_units = 8, .memory_mb = 64}).build());

    // Minnows jump the whale three times to trip the threshold.
    for (int i = 0; i < 3; ++i) {
        const JobId minnow = scheduler_->submit(
            JobSpecBuilder{}.priority(5)
                .resources({.cpu_units = 2, .memory_mb = 64}).build());
        tick();
        ASSERT_EQ(state(minnow), JobState::Dispatched);
        scheduler_->report_started(big, minnow);
        scheduler_->report_completion(big, minnow, {.success = true, .error = {}});
        tick();
    }
    ASSERT_TRUE(scheduler_->active_reservation().has_value());

    EXPECT_TRUE(scheduler_->cancel(whale));
    tick();

    EXPECT_FALSE(scheduler_->active_reservation().has_value());
    EXPECT_EQ(state(whale), JobState::Cancelled);
    (void)filler;
}

// ---------------------------------------------------------------------------
// Aging end-to-end through the scheduler's rescore tick
// ---------------------------------------------------------------------------

TEST_F(SchedulerTest, RescoreTickLetsStarvingJobOvertakeUnderComposite) {
    SchedulerConfig config;
    config.heartbeat_timeout = 1h;
    config.rescore_interval = 500ms;
    Scheduler scheduler(store_, registry_, bus_, clock_,
                        std::make_unique<CompositePolicy>(CompositePolicy::Weights{
                            .priority = 1.0, .urgency = 0.0,
                            .aging_per_sec = 0.05, .retry_penalty = 0.0}),
                        RetryConfig{.jitter = 0.0}, config);
    MockBackend backend;
    scheduler.attach_backend(backend);

    // No workers yet: jobs queue and age.
    const JobId old_low = scheduler.submit(JobSpecBuilder{}.priority(1).build());
    scheduler.run_once(clock_.now());
    clock_.advance(400s);  // old_low ages 400s -> +20 points.
    const JobId new_high = scheduler.submit(JobSpecBuilder{}.priority(9).build());
    scheduler.run_once(clock_.now());  // Rescore tick fires (500ms elapsed).

    // One 1-cpu worker appears: exactly one job can dispatch. It must be
    // the aged low-priority job, not the fresh high-priority one.
    registry_.register_worker("w1", {.cpu_units = 1, .memory_mb = 64});
    scheduler.run_once(clock_.now());

    ASSERT_EQ(backend.assignments.size(), 1u);
    EXPECT_EQ(backend.assignments[0].job_id, old_low);
    EXPECT_EQ(store_.get(new_high)->state, JobState::Queued);
}

}  // namespace
}  // namespace chronos
