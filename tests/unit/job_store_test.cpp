#include "chronos/core/job_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

namespace chronos {
namespace {

using namespace std::chrono_literals;

class JobStoreTest : public ::testing::Test {
protected:
    SimulatedClock clock_;
    EventBus bus_;
    JobStore store_{clock_, bus_};

    JobId submit_default() { return store_.submit(JobSpecBuilder{}.build()); }
};

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------

TEST_F(JobStoreTest, SubmitAssignsSequentialIdsAndSubmittedState) {
    const JobId a = submit_default();
    const JobId b = submit_default();

    EXPECT_TRUE(a.valid());
    EXPECT_TRUE(b.valid());
    EXPECT_NE(a, b);
    EXPECT_EQ(store_.size(), 2u);

    const auto job = store_.get(a);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->state, JobState::Submitted);
    EXPECT_EQ(job->attempt, 0);
    EXPECT_EQ(job->submit_time, clock_.now());
}

TEST_F(JobStoreTest, SubmitEmitsJobSubmittedEvent) {
    std::vector<Event> events;
    bus_.subscribe([&events](const Event& e) { events.push_back(e); });

    const JobId id = store_.submit(JobSpecBuilder{}.name("encode").build());

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::JobSubmitted);
    EXPECT_EQ(events[0].job_id, id);
    EXPECT_EQ(events[0].detail, "encode");
    EXPECT_EQ(events[0].timestamp, clock_.now());
}

TEST_F(JobStoreTest, GetUnknownJobReturnsNullopt) {
    EXPECT_FALSE(store_.get(JobId{999}).has_value());
}

// ---------------------------------------------------------------------------
// The happy path, with exact timeline verification
// ---------------------------------------------------------------------------

TEST_F(JobStoreTest, FullLifecycleRecordsExactTimeline) {
    const JobId id = submit_default();
    const TimePoint t0 = clock_.now();

    clock_.advance(1s);
    store_.transition(id, JobState::Queued);
    clock_.advance(2s);
    store_.transition(id, JobState::Dispatched);
    clock_.advance(3s);
    store_.transition(id, JobState::Running);
    clock_.advance(4s);
    store_.transition(id, JobState::Completed);

    const auto job = store_.get(id);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->state, JobState::Completed);
    EXPECT_EQ(job->attempt, 1);

    // The history is the execution timeline -- verify every hop and stamp.
    ASSERT_EQ(job->history.size(), 4u);

    EXPECT_EQ(job->history[0].from, JobState::Submitted);
    EXPECT_EQ(job->history[0].to, JobState::Queued);
    EXPECT_EQ(job->history[0].at, t0 + 1s);

    EXPECT_EQ(job->history[1].from, JobState::Queued);
    EXPECT_EQ(job->history[1].to, JobState::Dispatched);
    EXPECT_EQ(job->history[1].at, t0 + 3s);

    EXPECT_EQ(job->history[2].from, JobState::Dispatched);
    EXPECT_EQ(job->history[2].to, JobState::Running);
    EXPECT_EQ(job->history[2].at, t0 + 6s);

    EXPECT_EQ(job->history[3].from, JobState::Running);
    EXPECT_EQ(job->history[3].to, JobState::Completed);
    EXPECT_EQ(job->history[3].at, t0 + 10s);
}

TEST_F(JobStoreTest, TransitionEmitsJobStateChangedEvent) {
    const JobId id = submit_default();

    std::vector<Event> events;
    bus_.subscribe([&events](const Event& e) { events.push_back(e); });

    store_.transition(id, JobState::Queued);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::JobStateChanged);
    EXPECT_EQ(events[0].job_id, id);
    EXPECT_EQ(events[0].from_state, JobState::Submitted);
    EXPECT_EQ(events[0].to_state, JobState::Queued);
}

// ---------------------------------------------------------------------------
// Retry loop
// ---------------------------------------------------------------------------

TEST_F(JobStoreTest, RetryLoopIncrementsAttemptPerRunningEntry) {
    const JobId id = submit_default();

    store_.transition(id, JobState::Queued);
    store_.transition(id, JobState::Dispatched);
    store_.transition(id, JobState::Running);      // attempt 1
    store_.transition(id, JobState::RetryWait);    // failed, backoff
    store_.transition(id, JobState::Queued);
    store_.transition(id, JobState::Dispatched);
    store_.transition(id, JobState::Running);      // attempt 2
    store_.transition(id, JobState::Failed);       // retries exhausted

    const auto job = store_.get(id);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->state, JobState::Failed);
    EXPECT_EQ(job->attempt, 2);
    EXPECT_EQ(job->history.size(), 8u);
}

TEST_F(JobStoreTest, SetNextEligibleTimeStoresBackoffDeadline) {
    const JobId id = submit_default();
    const TimePoint eligible = clock_.now() + 2s;

    store_.set_next_eligible_time(id, eligible);

    const auto job = store_.get(id);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->next_eligible_time, eligible);
}

TEST_F(JobStoreTest, SetNextEligibleTimeOnUnknownJobThrows) {
    EXPECT_THROW(store_.set_next_eligible_time(JobId{404}, clock_.now()),
                 UnknownJobError);
}

// ---------------------------------------------------------------------------
// Worker-failure rescue paths
// ---------------------------------------------------------------------------

TEST_F(JobStoreTest, DispatchedJobMayReturnToQueuedWithoutConsumingAttempt) {
    const JobId id = submit_default();
    store_.transition(id, JobState::Queued);
    store_.transition(id, JobState::Dispatched);
    store_.transition(id, JobState::Queued);  // Worker died before starting.

    const auto job = store_.get(id);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->state, JobState::Queued);
    EXPECT_EQ(job->attempt, 0);  // Never actually ran.
}

TEST_F(JobStoreTest, RunningJobMayBeRescuedBackToQueued) {
    const JobId id = submit_default();
    store_.transition(id, JobState::Queued);
    store_.transition(id, JobState::Dispatched);
    store_.transition(id, JobState::Running);
    store_.transition(id, JobState::Queued);  // Heartbeat monitor rescue.

    const auto job = store_.get(id);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->state, JobState::Queued);
    EXPECT_EQ(job->attempt, 1);
}

// ---------------------------------------------------------------------------
// State machine enforcement
// ---------------------------------------------------------------------------

TEST_F(JobStoreTest, IllegalTransitionThrowsWithContext) {
    const JobId id = submit_default();
    try {
        store_.transition(id, JobState::Running);  // Submitted -> Running: no.
        FAIL() << "expected IllegalTransitionError";
    } catch (const IllegalTransitionError& e) {
        EXPECT_EQ(e.job_id(), id);
        EXPECT_EQ(e.from(), JobState::Submitted);
        EXPECT_EQ(e.to(), JobState::Running);
    }
}

TEST_F(JobStoreTest, TerminalStatesAreFinal) {
    const JobId id = submit_default();
    store_.transition(id, JobState::Queued);
    store_.transition(id, JobState::Dispatched);
    store_.transition(id, JobState::Running);
    store_.transition(id, JobState::Completed);

    EXPECT_THROW(store_.transition(id, JobState::Queued), IllegalTransitionError);
    EXPECT_THROW(store_.transition(id, JobState::Running), IllegalTransitionError);
    EXPECT_THROW(store_.transition(id, JobState::Cancelled), IllegalTransitionError);
}

TEST_F(JobStoreTest, AnyNonTerminalJobMayBeCancelled) {
    for (const JobState pre : {JobState::Submitted, JobState::Queued,
                               JobState::Dispatched, JobState::Running,
                               JobState::RetryWait}) {
        const JobId id = submit_default();
        // Walk the job to the target pre-state.
        if (pre != JobState::Submitted) {
            store_.transition(id, JobState::Queued);
        }
        if (pre == JobState::Dispatched || pre == JobState::Running ||
            pre == JobState::RetryWait) {
            store_.transition(id, JobState::Dispatched);
        }
        if (pre == JobState::Running || pre == JobState::RetryWait) {
            store_.transition(id, JobState::Running);
        }
        if (pre == JobState::RetryWait) {
            store_.transition(id, JobState::RetryWait);
        }

        store_.transition(id, JobState::Cancelled);
        EXPECT_EQ(store_.get(id)->state, JobState::Cancelled)
            << "cancel from " << to_string(pre);
    }
}

TEST_F(JobStoreTest, SelfTransitionIsIllegal) {
    const JobId id = submit_default();
    store_.transition(id, JobState::Queued);
    EXPECT_THROW(store_.transition(id, JobState::Queued), IllegalTransitionError);
}

TEST_F(JobStoreTest, TransitionOnUnknownJobThrows) {
    EXPECT_THROW(store_.transition(JobId{404}, JobState::Queued), UnknownJobError);
}

TEST_F(JobStoreTest, TransitionTableMatrixIsExactlyAsDocumented) {
    using S = JobState;
    const std::vector<S> all = {S::Submitted, S::Queued, S::Dispatched, S::Running,
                                S::RetryWait, S::Completed, S::Failed, S::Cancelled};

    const std::set<std::pair<S, S>> allowed = {
        {S::Submitted, S::Queued},     {S::Submitted, S::Cancelled},
        {S::Queued, S::Dispatched},    {S::Queued, S::Cancelled},
        {S::Dispatched, S::Running},   {S::Dispatched, S::Queued},
        {S::Dispatched, S::Cancelled},
        {S::Running, S::Completed},    {S::Running, S::RetryWait},
        {S::Running, S::Failed},       {S::Running, S::Queued},
        {S::Running, S::Cancelled},
        {S::RetryWait, S::Queued},     {S::RetryWait, S::Cancelled},
    };

    for (const S from : all) {
        for (const S to : all) {
            const bool expected = allowed.contains({from, to});
            EXPECT_EQ(JobStore::transition_allowed(from, to), expected)
                << to_string(from) << " -> " << to_string(to);
        }
    }
}

// ---------------------------------------------------------------------------
// Failure atomicity + counters
// ---------------------------------------------------------------------------

TEST_F(JobStoreTest, FailedTransitionLeavesJobUntouched) {
    const JobId id = submit_default();
    store_.transition(id, JobState::Queued);

    EXPECT_THROW(store_.transition(id, JobState::Completed), IllegalTransitionError);

    const auto job = store_.get(id);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->state, JobState::Queued);
    EXPECT_EQ(job->history.size(), 1u);  // Only the legal transition recorded.
}

TEST_F(JobStoreTest, CountTracksStates) {
    const JobId a = submit_default();
    const JobId b = submit_default();
    submit_default();  // stays Submitted

    store_.transition(a, JobState::Queued);
    store_.transition(b, JobState::Queued);
    store_.transition(b, JobState::Dispatched);

    EXPECT_EQ(store_.count(JobState::Submitted), 1u);
    EXPECT_EQ(store_.count(JobState::Queued), 1u);
    EXPECT_EQ(store_.count(JobState::Dispatched), 1u);
    EXPECT_EQ(store_.count(JobState::Completed), 0u);
}

// ---------------------------------------------------------------------------
// Concurrency smoke tests (real value under TSan)
// ---------------------------------------------------------------------------

TEST_F(JobStoreTest, ConcurrentSubmitsProduceUniqueIdsAndLoseNothing) {
    constexpr int kThreads = 8;
    constexpr int kJobsPerThread = 200;

    std::vector<std::vector<JobId>> per_thread_ids(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([this, t, &per_thread_ids] {
            for (int i = 0; i < kJobsPerThread; ++i) {
                per_thread_ids[static_cast<std::size_t>(t)].push_back(submit_default());
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    std::set<JobId> unique;
    for (const auto& ids : per_thread_ids) {
        unique.insert(ids.begin(), ids.end());
    }
    EXPECT_EQ(unique.size(), static_cast<std::size_t>(kThreads * kJobsPerThread));
    EXPECT_EQ(store_.size(), static_cast<std::size_t>(kThreads * kJobsPerThread));
}

TEST_F(JobStoreTest, ConcurrentTransitionsOnDisjointJobsAreSafe) {
    constexpr int kThreads = 4;
    constexpr int kJobsPerThread = 100;

    // Pre-submit all jobs, partitioned per thread.
    std::vector<std::vector<JobId>> partitions(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kJobsPerThread; ++i) {
            partitions[static_cast<std::size_t>(t)].push_back(submit_default());
        }
    }

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([this, &partitions, t] {
            for (const JobId id : partitions[static_cast<std::size_t>(t)]) {
                store_.transition(id, JobState::Queued);
                store_.transition(id, JobState::Dispatched);
                store_.transition(id, JobState::Running);
                store_.transition(id, JobState::Completed);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(store_.count(JobState::Completed),
              static_cast<std::size_t>(kThreads * kJobsPerThread));
}

}  // namespace
}  // namespace chronos
