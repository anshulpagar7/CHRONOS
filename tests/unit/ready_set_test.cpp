#include "chronos/scheduling/ready_set.h"

#include <gtest/gtest.h>

#include <chrono>

#include "chronos/scheduling/policies.h"

namespace chronos {
namespace {

using namespace std::chrono_literals;

Job make_job(std::uint64_t id, int priority, TimePoint submit) {
    Job job;
    job.id = JobId{id};
    job.spec = JobSpecBuilder{}.priority(priority).build();
    job.submit_time = submit;
    return job;
}

class ReadySetTest : public ::testing::Test {
protected:
    PriorityPolicy priority_policy_;
    TimePoint t0_ = TimePoint{} + 100s;
};

TEST_F(ReadySetTest, EmptySetHasNoBest) {
    ReadySet set(priority_policy_);
    EXPECT_TRUE(set.empty());
    EXPECT_FALSE(set.best().has_value());
}

TEST_F(ReadySetTest, BestReturnsHighestScore) {
    ReadySet set(priority_policy_);
    set.add(make_job(1, 3, t0_), t0_);
    set.add(make_job(2, 9, t0_), t0_);
    set.add(make_job(3, 5, t0_), t0_);

    EXPECT_EQ(set.size(), 3u);
    ASSERT_TRUE(set.best().has_value());
    EXPECT_EQ(*set.best(), JobId{2});
}

TEST_F(ReadySetTest, TiesBreakTowardOlderJobId) {
    ReadySet set(priority_policy_);
    set.add(make_job(7, 5, t0_), t0_);
    set.add(make_job(3, 5, t0_), t0_);  // Same priority, older id.

    EXPECT_EQ(*set.best(), JobId{3});
}

TEST_F(ReadySetTest, RemoveDeletesAndReports) {
    ReadySet set(priority_policy_);
    set.add(make_job(1, 9, t0_), t0_);
    set.add(make_job(2, 1, t0_), t0_);

    EXPECT_TRUE(set.remove(JobId{1}));
    EXPECT_FALSE(set.contains(JobId{1}));
    EXPECT_EQ(*set.best(), JobId{2});

    EXPECT_FALSE(set.remove(JobId{404}));  // Unknown: ignored, no throw.
    EXPECT_EQ(set.size(), 1u);
}

TEST_F(ReadySetTest, ReAddingRefreshesScoreInPlace) {
    ReadySet set(priority_policy_);
    set.add(make_job(1, 1, t0_), t0_);
    set.add(make_job(2, 5, t0_), t0_);
    EXPECT_EQ(*set.best(), JobId{2});

    set.add(make_job(1, 9, t0_), t0_);   // Same id, new priority.
    EXPECT_EQ(set.size(), 2u);           // No duplicate entry.
    EXPECT_EQ(*set.best(), JobId{1});
}

TEST_F(ReadySetTest, EntriesAreBestFirst) {
    ReadySet set(priority_policy_);
    set.add(make_job(1, 3, t0_), t0_);
    set.add(make_job(2, 9, t0_), t0_);
    set.add(make_job(3, 5, t0_), t0_);

    const auto entries = set.entries();
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].second, JobId{2});
    EXPECT_EQ(entries[1].second, JobId{3});
    EXPECT_EQ(entries[2].second, JobId{1});
    EXPECT_GE(entries[0].first, entries[1].first);
    EXPECT_GE(entries[1].first, entries[2].first);
}

// ---------------------------------------------------------------------------
// Rescoring: aging that actually works (v1's heap made this impossible)
// ---------------------------------------------------------------------------

TEST_F(ReadySetTest, RescoreWithAgingFlipsOrder) {
    // Old low-priority job vs fresh high-priority job under Composite.
    CompositePolicy composite{{.priority = 1.0, .urgency = 0,
                               .aging_per_sec = 0.05, .retry_penalty = 0}};
    ReadySet set(composite);

    const Job old_low = make_job(1, 1, t0_);
    const Job new_high = make_job(2, 9, t0_ + 400s);  // Arrives much later.
    set.add(old_low, t0_);
    set.add(new_high, t0_ + 400s);
    // At insertion time (t0+400s view): old_low = 1 + 20 age, but the set
    // still holds its stale t0 score of 1 vs new_high's 9.
    EXPECT_EQ(*set.best(), JobId{2});  // Priority wins on stale scores.

    // Rescore both at t0+400s: old_low = 1 + 0.05*400 = 21 > new_high = 9.
    const TimePoint later = t0_ + 400s;
    set.rescore({old_low, new_high}, later);
    EXPECT_EQ(*set.best(), JobId{1});  // The starving job aged its way up.
}

TEST_F(ReadySetTest, RescoreIgnoresUnknownSnapshotsAndKeepsMembers) {
    ReadySet set(priority_policy_);
    set.add(make_job(1, 5, t0_), t0_);

    // Snapshot for a job not in the set: ignored, not inserted.
    set.rescore({make_job(99, 9, t0_)}, t0_ + 1s);

    EXPECT_EQ(set.size(), 1u);
    EXPECT_FALSE(set.contains(JobId{99}));
    EXPECT_EQ(*set.best(), JobId{1});
}

// ---------------------------------------------------------------------------
// Eligibility scanning + skip accounting (backfill foundation)
// ---------------------------------------------------------------------------

TEST_F(ReadySetTest, BestWhereSkipsIneligibleAndCountsSkips) {
    ReadySet set(priority_policy_);
    set.add(make_job(1, 9, t0_), t0_);  // Best, but ineligible ("doesn't fit").
    set.add(make_job(2, 5, t0_), t0_);
    set.add(make_job(3, 1, t0_), t0_);

    const auto picked =
        set.best_where([](JobId id) { return id != JobId{1}; });

    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(*picked, JobId{2});
    EXPECT_EQ(set.skip_count(JobId{1}), 1);  // Passed over once.
    EXPECT_EQ(set.skip_count(JobId{2}), 0);  // Chosen, not skipped.
    EXPECT_EQ(set.skip_count(JobId{3}), 0);  // Never reached.
}

TEST_F(ReadySetTest, SkipCountsAccumulateAcrossScans) {
    ReadySet set(priority_policy_);
    set.add(make_job(1, 9, t0_), t0_);
    set.add(make_job(2, 5, t0_), t0_);

    for (int i = 0; i < 3; ++i) {
        (void)set.best_where([](JobId id) { return id != JobId{1}; });
    }
    EXPECT_EQ(set.skip_count(JobId{1}), 3);
}

TEST_F(ReadySetTest, SkipCountsSurviveRescoreButResetOnRemove) {
    CompositePolicy composite{};
    ReadySet set(composite);
    const Job a = make_job(1, 9, t0_);
    set.add(a, t0_);
    set.add(make_job(2, 5, t0_), t0_);

    (void)set.best_where([](JobId id) { return id != JobId{1}; });
    EXPECT_EQ(set.skip_count(JobId{1}), 1);

    set.rescore({a}, t0_ + 10s);
    EXPECT_EQ(set.skip_count(JobId{1}), 1);  // Rescore must not reset.

    set.remove(JobId{1});
    EXPECT_EQ(set.skip_count(JobId{1}), 0);  // Removal must reset.
}

TEST_F(ReadySetTest, BestWhereRespectsScanBound) {
    ReadySet set(priority_policy_);
    set.add(make_job(1, 9, t0_), t0_);
    set.add(make_job(2, 5, t0_), t0_);
    set.add(make_job(3, 1, t0_), t0_);  // Eligible, but beyond the bound.

    const auto picked = set.best_where(
        [](JobId id) { return id == JobId{3}; }, /*max_scan=*/2);

    EXPECT_FALSE(picked.has_value());
    // Nothing was picked, so nothing was jumped: no skips recorded.
    EXPECT_EQ(set.skip_count(JobId{1}), 0);
    EXPECT_EQ(set.skip_count(JobId{2}), 0);
    EXPECT_EQ(set.skip_count(JobId{3}), 0);  // Never examined.
}

TEST_F(ReadySetTest, BestWhereNothingEligible) {
    ReadySet set(priority_policy_);
    set.add(make_job(1, 9, t0_), t0_);

    const auto picked = set.best_where([](JobId) { return false; });
    EXPECT_FALSE(picked.has_value());
    // Saturation is not starvation: no pick -> no skips.
    EXPECT_EQ(set.skip_count(JobId{1}), 0);
}

}  // namespace
}  // namespace chronos
