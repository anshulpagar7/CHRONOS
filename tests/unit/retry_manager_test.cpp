#include "chronos/scheduling/retry_manager.h"

#include <gtest/gtest.h>

#include <chrono>

namespace chronos {
namespace {

using namespace std::chrono_literals;

Job failed_job(int attempt, int max_retries) {
    Job job;
    job.id = JobId{1};
    job.spec = JobSpecBuilder{}.max_retries(max_retries).build();
    job.attempt = attempt;
    return job;
}

RetryConfig no_jitter(Duration base = 1s, double multiplier = 2.0,
                      Duration max = 60s) {
    return RetryConfig{.base_backoff = base,
                       .multiplier = multiplier,
                       .max_backoff = max,
                       .jitter = 0.0};
}

const TimePoint kNow = TimePoint{} + 1000s;

// ---------------------------------------------------------------------------
// Retry vs give-up boundary (attempt semantics)
// ---------------------------------------------------------------------------

TEST(RetryManager, RetriesWhileAttemptsRemain) {
    RetryManager mgr(no_jitter());
    // max_retries = 2 -> up to 3 total runs. Attempts 1 and 2 retry.
    EXPECT_EQ(mgr.decide(failed_job(1, 2), kNow).action, RetryAction::Retry);
    EXPECT_EQ(mgr.decide(failed_job(2, 2), kNow).action, RetryAction::Retry);
}

TEST(RetryManager, GivesUpWhenRetriesExhausted) {
    RetryManager mgr(no_jitter());
    // Attempt 3 of max_retries=2 was the last permitted run.
    EXPECT_EQ(mgr.decide(failed_job(3, 2), kNow).action, RetryAction::GiveUp);
}

TEST(RetryManager, ZeroMaxRetriesGivesUpImmediately) {
    RetryManager mgr(no_jitter());
    EXPECT_EQ(mgr.decide(failed_job(1, 0), kNow).action, RetryAction::GiveUp);
}

// ---------------------------------------------------------------------------
// Exponential backoff, exact with jitter disabled
// ---------------------------------------------------------------------------

TEST(RetryManager, BackoffDoublesPerAttempt) {
    RetryManager mgr(no_jitter(1s, 2.0));

    const auto d1 = mgr.decide(failed_job(1, 10), kNow);
    const auto d2 = mgr.decide(failed_job(2, 10), kNow);
    const auto d3 = mgr.decide(failed_job(3, 10), kNow);
    const auto d4 = mgr.decide(failed_job(4, 10), kNow);

    EXPECT_EQ(d1.delay, 1s);
    EXPECT_EQ(d2.delay, 2s);
    EXPECT_EQ(d3.delay, 4s);
    EXPECT_EQ(d4.delay, 8s);
    EXPECT_EQ(d1.eligible_at, kNow + 1s);
    EXPECT_EQ(d4.eligible_at, kNow + 8s);
}

TEST(RetryManager, BackoffIsCappedAtMax) {
    RetryManager mgr(no_jitter(1s, 2.0, 5s));

    EXPECT_EQ(mgr.decide(failed_job(3, 50), kNow).delay, 4s);   // Under cap.
    EXPECT_EQ(mgr.decide(failed_job(4, 50), kNow).delay, 5s);   // 8s -> capped.
    EXPECT_EQ(mgr.decide(failed_job(30, 50), kNow).delay, 5s);  // 2^29 s -> capped, no overflow.
}

TEST(RetryManager, MultiplierOneGivesConstantBackoff) {
    RetryManager mgr(no_jitter(3s, 1.0));
    EXPECT_EQ(mgr.decide(failed_job(1, 10), kNow).delay, 3s);
    EXPECT_EQ(mgr.decide(failed_job(7, 10), kNow).delay, 3s);
}

// ---------------------------------------------------------------------------
// Jitter
// ---------------------------------------------------------------------------

TEST(RetryManager, JitterStaysWithinConfiguredBand) {
    RetryConfig cfg{.base_backoff = 10s, .multiplier = 1.0,
                    .max_backoff = 60s, .jitter = 0.2, .seed = 7};
    RetryManager mgr(cfg);

    for (int i = 0; i < 200; ++i) {
        const auto d = mgr.decide(failed_job(1, 10), kNow);
        EXPECT_GE(d.delay, 8s);   // 10s * (1 - 0.2)
        EXPECT_LE(d.delay, 12s);  // 10s * (1 + 0.2)
    }
}

TEST(RetryManager, JitterIsDeterministicForSameSeed) {
    RetryConfig cfg{.base_backoff = 10s, .multiplier = 2.0,
                    .max_backoff = 600s, .jitter = 0.3, .seed = 1234};
    RetryManager a(cfg);
    RetryManager b(cfg);

    for (int attempt = 1; attempt <= 5; ++attempt) {
        EXPECT_EQ(a.decide(failed_job(attempt, 10), kNow).delay,
                  b.decide(failed_job(attempt, 10), kNow).delay);
    }
}

TEST(RetryManager, JitterNeverExceedsMaxBackoff) {
    RetryConfig cfg{.base_backoff = 10s, .multiplier = 1.0,
                    .max_backoff = 10s, .jitter = 0.5, .seed = 9};
    RetryManager mgr(cfg);
    for (int i = 0; i < 100; ++i) {
        EXPECT_LE(mgr.decide(failed_job(1, 5), kNow).delay, 10s);
    }
}

// ---------------------------------------------------------------------------
// Config validation
// ---------------------------------------------------------------------------

TEST(RetryManager, RejectsInvalidConfig) {
    EXPECT_THROW(RetryManager({.multiplier = 0.5}), std::invalid_argument);
    EXPECT_THROW(RetryManager({.jitter = 1.0}), std::invalid_argument);
    EXPECT_THROW(RetryManager({.jitter = -0.1}), std::invalid_argument);
    EXPECT_THROW(RetryManager({.base_backoff = -1s}), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Pending-retry queue
// ---------------------------------------------------------------------------

TEST(RetryManager, CollectDueReturnsOnlyRipeJobsInOrder) {
    RetryManager mgr(no_jitter());
    mgr.schedule(JobId{1}, kNow + 5s);
    mgr.schedule(JobId{2}, kNow + 1s);
    mgr.schedule(JobId{3}, kNow + 3s);
    EXPECT_EQ(mgr.pending(), 3u);

    const auto due = mgr.collect_due(kNow + 3s);
    ASSERT_EQ(due.size(), 2u);
    EXPECT_EQ(due[0], JobId{2});  // Eligibility order.
    EXPECT_EQ(due[1], JobId{3});  // Due at exactly `now` counts as due.
    EXPECT_EQ(mgr.pending(), 1u);
}

TEST(RetryManager, CollectDueOnEmptyOrUnripeQueue) {
    RetryManager mgr(no_jitter());
    EXPECT_TRUE(mgr.collect_due(kNow).empty());

    mgr.schedule(JobId{1}, kNow + 10s);
    EXPECT_TRUE(mgr.collect_due(kNow).empty());
    EXPECT_EQ(mgr.pending(), 1u);
}

TEST(RetryManager, NextDueReportsSoonestEligibility) {
    RetryManager mgr(no_jitter());
    EXPECT_FALSE(mgr.next_due().has_value());

    mgr.schedule(JobId{1}, kNow + 5s);
    mgr.schedule(JobId{2}, kNow + 1s);

    ASSERT_TRUE(mgr.next_due().has_value());
    EXPECT_EQ(*mgr.next_due(), kNow + 1s);

    (void)mgr.collect_due(kNow + 1s);
    EXPECT_EQ(*mgr.next_due(), kNow + 5s);
}

// ---------------------------------------------------------------------------
// End-to-end shape: decide -> schedule -> collect, on a simulated clock
// ---------------------------------------------------------------------------

TEST(RetryManager, FullBackoffCycleWithSimulatedClock) {
    SimulatedClock clock{kNow};
    RetryManager mgr(no_jitter(2s));

    const auto decision = mgr.decide(failed_job(1, 3), clock.now());
    ASSERT_EQ(decision.action, RetryAction::Retry);
    mgr.schedule(JobId{1}, decision.eligible_at);

    clock.advance(1999ms);
    EXPECT_TRUE(mgr.collect_due(clock.now()).empty());  // 1ms too early.

    clock.advance(1ms);
    const auto due = mgr.collect_due(clock.now());      // Exactly on time.
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0], JobId{1});
}

}  // namespace
}  // namespace chronos
