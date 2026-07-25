#include "chronos/observability/timeline_recorder.h"

#include <gtest/gtest.h>

#include "chronos/core/job_store.h"

namespace chronos {
namespace {

using namespace std::chrono_literals;

TEST(TimelineRecorder, RecordsPerJobViewsInOrder) {
    SimulatedClock clock{TimePoint{} + 100s};
    EventBus bus;
    TimelineRecorder recorder(bus);
    JobStore store(clock, bus);

    const JobId a = store.submit(JobSpecBuilder{}.name("a").build());
    const JobId b = store.submit(JobSpecBuilder{}.name("b").build());
    store.transition(a, JobState::Queued);
    store.transition(b, JobState::Queued);
    store.transition(a, JobState::Dispatched);

    const auto for_a = recorder.for_job(a);
    ASSERT_EQ(for_a.size(), 3u);  // submitted, ->Queued, ->Dispatched.
    EXPECT_EQ(for_a[0].type, EventType::JobSubmitted);
    EXPECT_EQ(for_a[2].to_state, JobState::Dispatched);

    EXPECT_EQ(recorder.for_job(b).size(), 2u);
    EXPECT_TRUE(recorder.for_job(JobId{99}).empty());
    EXPECT_EQ(recorder.size(), 5u);
}

TEST(TimelineRecorder, RecentReturnsTailOldestFirst) {
    SimulatedClock clock;
    EventBus bus;
    TimelineRecorder recorder(bus);
    JobStore store(clock, bus);

    const JobId id = store.submit(JobSpecBuilder{}.build());
    store.transition(id, JobState::Queued);
    store.transition(id, JobState::Dispatched);

    const auto tail = recorder.recent(2);
    ASSERT_EQ(tail.size(), 2u);
    EXPECT_EQ(tail[0].to_state, JobState::Queued);
    EXPECT_EQ(tail[1].to_state, JobState::Dispatched);

    EXPECT_EQ(recorder.recent(100).size(), 3u);  // Clamped to available.
}

TEST(TimelineRecorder, RingEvictsOldestAtCapacity) {
    SimulatedClock clock;
    EventBus bus;
    TimelineRecorder recorder(bus, /*max_events=*/3);
    JobStore store(clock, bus);

    const JobId a = store.submit(JobSpecBuilder{}.build());  // Evicted later.
    store.transition(a, JobState::Queued);
    store.transition(a, JobState::Dispatched);
    store.transition(a, JobState::Running);  // 4th event: submit evicted.

    EXPECT_EQ(recorder.size(), 3u);
    const auto for_a = recorder.for_job(a);
    ASSERT_EQ(for_a.size(), 3u);
    EXPECT_EQ(for_a[0].type, EventType::JobStateChanged);  // Submit is gone.
    EXPECT_EQ(for_a[0].to_state, JobState::Queued);
}

}  // namespace
}  // namespace chronos
