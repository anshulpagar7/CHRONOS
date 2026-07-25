#include "chronos/core/clock.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

namespace chronos {
namespace {

using namespace std::chrono_literals;

TEST(SimulatedClock, StartsAtGivenTime) {
    const TimePoint start = TimePoint{} + 100s;
    SimulatedClock clock{start};
    EXPECT_EQ(clock.now(), start);
}

TEST(SimulatedClock, TimeIsFrozenUntilAdvanced) {
    SimulatedClock clock;
    const TimePoint before = clock.now();
    std::this_thread::sleep_for(5ms);  // Real time passes...
    EXPECT_EQ(clock.now(), before);    // ...simulated time does not.
}

TEST(SimulatedClock, AdvanceMovesTimeExactly) {
    SimulatedClock clock;
    const TimePoint start = clock.now();
    clock.advance(2s);
    EXPECT_EQ(clock.now(), start + 2s);
    clock.advance(500ms);
    EXPECT_EQ(clock.now(), start + 2500ms);
}

TEST(SimulatedClock, AdvanceToJumpsToTarget) {
    SimulatedClock clock;
    const TimePoint target = clock.now() + 1h;
    clock.advance_to(target);
    EXPECT_EQ(clock.now(), target);
}

TEST(SimulatedClock, RejectsNegativeAdvance) {
    SimulatedClock clock;
    EXPECT_THROW(clock.advance(-1ns), std::invalid_argument);
}

TEST(SimulatedClock, RejectsAdvanceToThePast) {
    SimulatedClock clock;
    clock.advance(10s);
    EXPECT_THROW(clock.advance_to(clock.now() - 1s), std::invalid_argument);
}

TEST(SimulatedClock, ConcurrentReadersSeeMonotonicTime) {
    SimulatedClock clock;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;

    // 4 reader threads continuously verify time never goes backwards
    // while the main thread advances. TSan hammers this path.
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&clock, &stop] {
            TimePoint last = clock.now();
            while (!stop.load(std::memory_order_relaxed)) {
                const TimePoint t = clock.now();
                ASSERT_GE(t, last);
                last = t;
            }
        });
    }

    for (int i = 0; i < 1000; ++i) {
        clock.advance(1ms);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) {
        t.join();
    }
    EXPECT_EQ(clock.now(), TimePoint{} + 1000ms);
}

TEST(SystemClock, IsMonotonicallyNonDecreasing) {
    SystemClock clock;
    const TimePoint a = clock.now();
    const TimePoint b = clock.now();
    EXPECT_LE(a, b);
}

}  // namespace
}  // namespace chronos
