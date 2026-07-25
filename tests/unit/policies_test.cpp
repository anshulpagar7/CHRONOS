#include "chronos/scheduling/policies.h"

#include <gtest/gtest.h>

#include <chrono>

#include "chronos/core/clock.h"

namespace chronos {
namespace {

using namespace std::chrono_literals;

Job make_job(std::uint64_t id, JobSpec spec, TimePoint submit) {
    Job job;
    job.id = JobId{id};
    job.spec = std::move(spec);
    job.submit_time = submit;
    return job;
}

// ---------------------------------------------------------------------------
// FIFO
// ---------------------------------------------------------------------------

TEST(FifoPolicy, EarlierSubmissionScoresHigher) {
    FifoPolicy policy;
    const TimePoint t0 = TimePoint{} + 100s;
    const Job early = make_job(1, JobSpecBuilder{}.build(), t0);
    const Job late = make_job(2, JobSpecBuilder{}.build(), t0 + 5s);

    const TimePoint now = t0 + 10s;
    EXPECT_GT(policy.score(early, now), policy.score(late, now));
}

TEST(FifoPolicy, ScoreIgnoresPriority) {
    FifoPolicy policy;
    const TimePoint t0 = TimePoint{} + 100s;
    const Job low = make_job(1, JobSpecBuilder{}.priority(0).build(), t0);
    const Job high = make_job(2, JobSpecBuilder{}.priority(9).build(), t0 + 1ns);

    EXPECT_GT(policy.score(low, t0 + 1s), policy.score(high, t0 + 1s));
}

// ---------------------------------------------------------------------------
// Priority
// ---------------------------------------------------------------------------

TEST(PriorityPolicy, HigherPriorityScoresHigher) {
    PriorityPolicy policy;
    const TimePoint t0 = TimePoint{} + 100s;
    const Job low = make_job(1, JobSpecBuilder{}.priority(1).build(), t0);
    const Job high = make_job(2, JobSpecBuilder{}.priority(9).build(), t0);

    EXPECT_GT(policy.score(high, t0), policy.score(low, t0));
}

// ---------------------------------------------------------------------------
// EDF
// ---------------------------------------------------------------------------

TEST(EdfPolicy, EarlierDeadlineScoresHigher) {
    EdfPolicy policy;
    const TimePoint t0 = TimePoint{} + 100s;
    const Job soon = make_job(1, JobSpecBuilder{}.deadline(t0 + 10s).build(), t0);
    const Job later = make_job(2, JobSpecBuilder{}.deadline(t0 + 60s).build(), t0);

    EXPECT_GT(policy.score(soon, t0), policy.score(later, t0));
}

TEST(EdfPolicy, OverdueJobOutranksUpcomingJob) {
    // The exact case v1 got backwards: overdue must rank HIGHEST.
    EdfPolicy policy;
    const TimePoint t0 = TimePoint{} + 100s;
    const Job overdue = make_job(1, JobSpecBuilder{}.deadline(t0 - 5s).build(), t0 - 60s);
    const Job upcoming = make_job(2, JobSpecBuilder{}.deadline(t0 + 1ms).build(), t0 - 60s);

    EXPECT_GT(policy.score(overdue, t0), policy.score(upcoming, t0));
}

TEST(EdfPolicy, NoDeadlineRanksBelowAnyDeadline) {
    EdfPolicy policy;
    const TimePoint t0 = TimePoint{} + 100s;
    const Job none = make_job(1, JobSpecBuilder{}.build(), t0);
    const Job far = make_job(2, JobSpecBuilder{}.deadline(t0 + 10000h).build(), t0);

    EXPECT_LT(policy.score(none, t0), policy.score(far, t0));
}

// ---------------------------------------------------------------------------
// Composite
// ---------------------------------------------------------------------------

TEST(CompositePolicy, PriorityRaisesScore) {
    CompositePolicy policy;
    const TimePoint t0 = TimePoint{} + 100s;
    const Job low = make_job(1, JobSpecBuilder{}.priority(1).build(), t0);
    const Job high = make_job(2, JobSpecBuilder{}.priority(5).build(), t0);

    EXPECT_GT(policy.score(high, t0), policy.score(low, t0));
}

TEST(CompositePolicy, UrgencyIsBoundedAtDeadlineAndBeyond) {
    // With only the urgency term active, the score must approach -- and
    // once overdue, clamp at -- exactly w_urgency. No 1/x blow-up (v1 bug).
    const double w_urgency = 10.0;
    CompositePolicy policy{{.priority = 0, .urgency = w_urgency,
                            .aging_per_sec = 0, .retry_penalty = 0}};
    const TimePoint t0 = TimePoint{} + 1000s;
    const Job job = make_job(1, JobSpecBuilder{}.deadline(t0).build(), t0 - 30s);

    const double at_deadline = policy.score(job, t0);
    const double overdue_1s = policy.score(job, t0 + 1s);
    const double overdue_1h = policy.score(job, t0 + 1h);

    EXPECT_DOUBLE_EQ(at_deadline, w_urgency);  // horizon/(horizon+0) = 1
    EXPECT_DOUBLE_EQ(overdue_1s, w_urgency);   // clamped, not diverging
    EXPECT_DOUBLE_EQ(overdue_1h, w_urgency);
}

TEST(CompositePolicy, UrgencyGrowsAsDeadlineApproaches) {
    CompositePolicy policy{{.priority = 0, .urgency = 10.0,
                            .aging_per_sec = 0, .retry_penalty = 0}};
    const TimePoint t0 = TimePoint{} + 1000s;
    const Job job = make_job(1, JobSpecBuilder{}.deadline(t0 + 120s).build(), t0);

    const double far = policy.score(job, t0);
    const double near = policy.score(job, t0 + 100s);
    EXPECT_GT(near, far);
}

TEST(CompositePolicy, AgingRaisesScoreOverTime) {
    CompositePolicy policy{{.priority = 1.0, .urgency = 0,
                            .aging_per_sec = 0.05, .retry_penalty = 0}};
    const TimePoint t0 = TimePoint{} + 100s;
    const Job job = make_job(1, JobSpecBuilder{}.priority(1).build(), t0);

    const double fresh = policy.score(job, t0);
    const double aged = policy.score(job, t0 + 100s);

    EXPECT_DOUBLE_EQ(fresh, 1.0);
    EXPECT_DOUBLE_EQ(aged, 1.0 + 0.05 * 100.0);
}

TEST(CompositePolicy, AgingEventuallyBeatsAnyPriorityGap) {
    // Starvation freedom: unbounded aging must overtake a bounded
    // priority advantage, given enough waiting.
    CompositePolicy policy{{.priority = 1.0, .urgency = 0,
                            .aging_per_sec = 0.05, .retry_penalty = 0}};
    const TimePoint t0 = TimePoint{} + 100s;
    const Job old_low = make_job(1, JobSpecBuilder{}.priority(1).build(), t0);
    const Job new_high = make_job(2, JobSpecBuilder{}.priority(9).build(), t0 + 400s);

    // At t0+400s the gap is 8 priority points vs 400s * 0.05 = 20 age points.
    const TimePoint now = t0 + 400s;
    EXPECT_GT(policy.score(old_low, now), policy.score(new_high, now));
}

TEST(CompositePolicy, RetryPenaltyLowersScore) {
    CompositePolicy policy{{.priority = 0, .urgency = 0,
                            .aging_per_sec = 0, .retry_penalty = 0.5}};
    const TimePoint t0 = TimePoint{} + 100s;
    Job fresh = make_job(1, JobSpecBuilder{}.build(), t0);
    Job retried = make_job(2, JobSpecBuilder{}.build(), t0);
    retried.attempt = 3;

    EXPECT_GT(policy.score(fresh, t0), policy.score(retried, t0));
    EXPECT_DOUBLE_EQ(policy.score(retried, t0), -1.5);
}

TEST(CompositePolicy, DeterministicForSameInputs) {
    CompositePolicy policy;
    const TimePoint t0 = TimePoint{} + 100s;
    const Job job =
        make_job(1, JobSpecBuilder{}.priority(3).deadline(t0 + 30s).build(), t0);

    EXPECT_DOUBLE_EQ(policy.score(job, t0 + 5s), policy.score(job, t0 + 5s));
}

}  // namespace
}  // namespace chronos
