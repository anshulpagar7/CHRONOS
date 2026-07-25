#include "chronos/core/ids.h"

#include <gtest/gtest.h>

#include <type_traits>
#include <unordered_map>

namespace chronos {
namespace {

TEST(StrongId, DefaultConstructedIsInvalid) {
    JobId id;
    EXPECT_FALSE(id.valid());
    EXPECT_EQ(id.value(), 0u);
}

TEST(StrongId, ExplicitValueIsValid) {
    JobId id{42};
    EXPECT_TRUE(id.valid());
    EXPECT_EQ(id.value(), 42u);
}

TEST(StrongId, ComparisonOperators) {
    EXPECT_EQ(JobId{7}, JobId{7});
    EXPECT_NE(JobId{7}, JobId{8});
    EXPECT_LT(JobId{7}, JobId{8});
}

TEST(StrongId, DistinctTagsAreDistinctTypes) {
    static_assert(!std::is_same_v<JobId, WorkerId>,
                  "JobId and WorkerId must not be interchangeable");
}

TEST(StrongId, UsableAsUnorderedMapKey) {
    std::unordered_map<JobId, int> map;
    map[JobId{1}] = 10;
    map[JobId{2}] = 20;
    EXPECT_EQ(map.at(JobId{1}), 10);
    EXPECT_EQ(map.at(JobId{2}), 20);
    EXPECT_EQ(map.size(), 2u);
}

}  // namespace
}  // namespace chronos
