#pragma once

#include <cstdint>
#include <optional>
#include <queue>
#include <random>
#include <utility>
#include <vector>

#include "chronos/core/clock.h"
#include "chronos/core/ids.h"
#include "chronos/core/job.h"

namespace chronos {

/// Tunables for retry behaviour.
struct RetryConfig {
    Duration base_backoff = std::chrono::seconds(1);  ///< Delay before retry #1.
    double multiplier = 2.0;                          ///< Growth per subsequent retry.
    Duration max_backoff = std::chrono::seconds(60);  ///< Hard delay ceiling.
    /// Jitter fraction j: the computed delay is scaled by a factor drawn
    /// uniformly from [1-j, 1+j]. Jitter prevents retry storms -- a batch
    /// of jobs failing together (e.g. one worker dying) would otherwise
    /// all come back at the same instant. 0 disables.
    double jitter = 0.1;
    /// RNG seed. Fixed by default so runs are reproducible (simulator!);
    /// pass entropy in production if desired.
    std::uint64_t seed = 0x5EED;
};

enum class RetryAction : std::uint8_t {
    Retry,    ///< Re-queue after RetryDecision::eligible_at.
    GiveUp,   ///< Retries exhausted: job goes to Failed (dead-letter).
};

struct RetryDecision {
    RetryAction action;
    Duration delay{};         ///< Backoff applied (Retry only).
    TimePoint eligible_at{};  ///< now + delay (Retry only).
};

/// Owns retry semantics end to end:
///  * decide(): given a just-failed job, retry (with what backoff) or give
///    up? Pure policy over (attempt, max_retries, config) + deterministic
///    jitter -- exactly unit-testable with jitter = 0.
///  * a pending queue of (eligible_at, id): the scheduler schedules
///    decided retries here and drains the due ones each loop, using
///    next_due() as its condition-variable wait deadline.
///
/// Attempt semantics (matches JobStore: attempt = entries into Running):
/// spec.max_retries is *additional* attempts after the first, so a job may
/// run at most max_retries + 1 times. A failure with attempt <= max_retries
/// retries; attempt == max_retries + 1 gives up.
///
/// Not internally synchronized: owned by the single scheduler thread.
class RetryManager {
public:
    explicit RetryManager(RetryConfig config = {});

    /// Decide the fate of `job`, which just failed its `job.attempt`-th run.
    [[nodiscard]] RetryDecision decide(const Job& job, TimePoint now);

    /// Park a job until `eligible_at`.
    void schedule(JobId id, TimePoint eligible_at);

    /// Pop and return every job whose eligibility time has arrived
    /// (eligible_at <= now), in eligibility order.
    [[nodiscard]] std::vector<JobId> collect_due(TimePoint now);

    /// Eligibility time of the soonest pending retry, if any. The scheduler
    /// sleeps at most until this instant.
    [[nodiscard]] std::optional<TimePoint> next_due() const;

    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }

    [[nodiscard]] const RetryConfig& config() const noexcept { return config_; }

private:
    /// Backoff for the retry following attempt #`attempt`, before jitter:
    /// min(base * multiplier^(attempt-1), max_backoff).
    [[nodiscard]] Duration base_delay_for_attempt(int attempt) const;

    RetryConfig config_;
    std::mt19937_64 rng_;

    struct QueueEntry {
        TimePoint eligible_at;
        JobId id;
        bool operator>(const QueueEntry& other) const noexcept {
            if (eligible_at != other.eligible_at) {
                return eligible_at > other.eligible_at;
            }
            return id.value() > other.id.value();
        }
    };
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<>> queue_;
};

}  // namespace chronos
