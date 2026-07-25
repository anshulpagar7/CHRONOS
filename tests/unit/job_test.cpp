#include "chronos/core/job.h"

#include <gtest/gtest.h>

#include <string>

namespace chronos {
namespace {

TEST(JobSpecBuilder, ProvidesSaneDefaults) {
    const JobSpec spec = JobSpecBuilder{}.build();
    EXPECT_EQ(spec.name, "job");
    EXPECT_EQ(spec.priority, 0);
    EXPECT_FALSE(spec.deadline.has_value());
    EXPECT_EQ(spec.max_retries, 0);
    EXPECT_EQ(spec.resources.cpu_units, 1u);
    EXPECT_EQ(spec.resources.memory_mb, 64u);
    EXPECT_TRUE(spec.payload.empty());
}

TEST(JobSpecBuilder, SetsAllFields) {
    const TimePoint dl = TimePoint{} + std::chrono::seconds(30);
    const JobSpec spec = JobSpecBuilder{}
                             .name("encode-video")
                             .priority(5)
                             .deadline(dl)
                             .max_retries(3)
                             .resources({.cpu_units = 2, .memory_mb = 512})
                             .payload("input.mp4")
                             .build();

    EXPECT_EQ(spec.name, "encode-video");
    EXPECT_EQ(spec.priority, 5);
    ASSERT_TRUE(spec.deadline.has_value());
    EXPECT_EQ(*spec.deadline, dl);
    EXPECT_EQ(spec.max_retries, 3);
    EXPECT_EQ(spec.resources.cpu_units, 2u);
    EXPECT_EQ(spec.resources.memory_mb, 512u);
    EXPECT_EQ(spec.payload, "input.mp4");
}

TEST(JobSpecBuilder, RejectsEmptyName) {
    EXPECT_THROW(JobSpecBuilder{}.name("").build(), std::invalid_argument);
}

TEST(JobSpecBuilder, RejectsNegativeRetries) {
    EXPECT_THROW(JobSpecBuilder{}.max_retries(-1).build(), std::invalid_argument);
}

TEST(JobSpecBuilder, RejectsZeroCpu) {
    EXPECT_THROW(JobSpecBuilder{}.resources({.cpu_units = 0, .memory_mb = 64}).build(),
                 std::invalid_argument);
}

TEST(JobSpecBuilder, RejectsZeroMemory) {
    EXPECT_THROW(JobSpecBuilder{}.resources({.cpu_units = 1, .memory_mb = 0}).build(),
                 std::invalid_argument);
}

TEST(JobState, TerminalStatesAreExactlyCompletedFailedCancelled) {
    EXPECT_TRUE(is_terminal(JobState::Completed));
    EXPECT_TRUE(is_terminal(JobState::Failed));
    EXPECT_TRUE(is_terminal(JobState::Cancelled));

    EXPECT_FALSE(is_terminal(JobState::Submitted));
    EXPECT_FALSE(is_terminal(JobState::Queued));
    EXPECT_FALSE(is_terminal(JobState::Dispatched));
    EXPECT_FALSE(is_terminal(JobState::Running));
    EXPECT_FALSE(is_terminal(JobState::RetryWait));
}

TEST(JobState, ToStringCoversAllStates) {
    EXPECT_STREQ(to_string(JobState::Submitted), "SUBMITTED");
    EXPECT_STREQ(to_string(JobState::Queued), "QUEUED");
    EXPECT_STREQ(to_string(JobState::Dispatched), "DISPATCHED");
    EXPECT_STREQ(to_string(JobState::Running), "RUNNING");
    EXPECT_STREQ(to_string(JobState::RetryWait), "RETRY_WAIT");
    EXPECT_STREQ(to_string(JobState::Completed), "COMPLETED");
    EXPECT_STREQ(to_string(JobState::Failed), "FAILED");
    EXPECT_STREQ(to_string(JobState::Cancelled), "CANCELLED");
}

}  // namespace
}  // namespace chronos
