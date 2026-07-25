#pragma once

#include <chrono>
#include <mutex>
#include <stdexcept>

namespace chronos {

using Duration  = std::chrono::nanoseconds;
using TimePoint = std::chrono::steady_clock::time_point;

/// Abstract time source.
///
/// Every component in CHRONOS that needs the current time receives a Clock&
/// instead of calling std::chrono::steady_clock::now() directly. This is the
/// single decision that makes retry backoff, deadline scheduling, heartbeat
/// timeouts, and the entire simulator deterministic and unit-testable.
/// See docs/decisions/ADR-001-clock-injection.md.
class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual TimePoint now() const noexcept = 0;
};

/// Production clock backed by std::chrono::steady_clock (monotonic --
/// immune to NTP adjustments and wall-clock changes).
class SystemClock final : public Clock {
public:
    [[nodiscard]] TimePoint now() const noexcept override {
        return std::chrono::steady_clock::now();
    }
};

/// Fully controllable clock for tests and the scheduling simulator.
///
/// Time only moves when explicitly advanced, so a test can assert
/// "this job becomes eligible after *exactly* 2s of backoff" with zero
/// flakiness, and the simulator can push 10k jobs through the real
/// scheduler in milliseconds of wall time.
///
/// Thread-safe: multiple worker threads may call now() while a driver
/// thread advances time.
class SimulatedClock final : public Clock {
public:
    explicit SimulatedClock(TimePoint start = TimePoint{}) : now_(start) {}

    [[nodiscard]] TimePoint now() const noexcept override {
        std::lock_guard lock(mutex_);
        return now_;
    }

    /// Move time forward by `delta`. Negative deltas are rejected --
    /// a monotonic clock never runs backwards.
    void advance(Duration delta) {
        if (delta < Duration::zero()) {
            throw std::invalid_argument("SimulatedClock::advance: negative delta");
        }
        std::lock_guard lock(mutex_);
        now_ += delta;
    }

    /// Jump directly to `target`. Must not be earlier than the current time.
    void advance_to(TimePoint target) {
        std::lock_guard lock(mutex_);
        if (target < now_) {
            throw std::invalid_argument("SimulatedClock::advance_to: target is in the past");
        }
        now_ = target;
    }

private:
    mutable std::mutex mutex_;
    TimePoint now_;
};

}  // namespace chronos
