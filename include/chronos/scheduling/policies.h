#pragma once

#include "chronos/scheduling/scheduling_policy.h"

namespace chronos {

/// First-in, first-out: earlier submission always wins.
/// Score is now-independent: the negated submit timestamp.
class FifoPolicy final : public SchedulingPolicy {
public:
    [[nodiscard]] double score(const Job& job, TimePoint now) const override;
    [[nodiscard]] const char* name() const noexcept override { return "fifo"; }
};

/// Strict priority: higher spec.priority wins. Ties are broken by the
/// ReadySet's stable id ordering (older id first), giving FIFO within a
/// priority band.
class PriorityPolicy final : public SchedulingPolicy {
public:
    [[nodiscard]] double score(const Job& job, TimePoint now) const override;
    [[nodiscard]] const char* name() const noexcept override { return "priority"; }
};

/// Earliest Deadline First. Jobs without a deadline rank below every job
/// that has one (-infinity). Overdue jobs naturally rank highest -- the
/// more overdue, the earlier the deadline, the higher the score. (v1 got
/// this exactly backwards by scoring 1/time_left, which vanished for
/// overdue jobs and diverged near the deadline.)
class EdfPolicy final : public SchedulingPolicy {
public:
    [[nodiscard]] double score(const Job& job, TimePoint now) const override;
    [[nodiscard]] const char* name() const noexcept override { return "edf"; }
};

/// Weighted multi-factor policy -- the CHRONOS default, resembling how
/// production schedulers blend concerns:
///
///   score = w_priority · priority
///         + w_urgency  · urgency(deadline, now)      // bounded (0, 1]
///         + w_aging    · seconds_waiting             // starvation prevention
///         - w_retry    · attempts_consumed           // repeat offenders yield
///
/// Urgency is deliberately *bounded*: horizon / (horizon + time_left),
/// which tends to 1 as the deadline approaches and clamps at 1 once
/// overdue. An unbounded 1/time_left term (v1) diverges near the deadline
/// and drowns every other factor; a bounded one keeps the weights
/// meaningful. Jobs without a deadline contribute 0 urgency.
///
/// Aging grows linearly and without bound, so any job eventually
/// outscores everything -- a hard starvation-freedom guarantee.
class CompositePolicy final : public SchedulingPolicy {
public:
    struct Weights {
        double priority = 1.0;
        double urgency = 10.0;
        double aging_per_sec = 0.05;
        double retry_penalty = 0.5;
    };

    /// Default weights, 60s urgency horizon.
    CompositePolicy();
    explicit CompositePolicy(Weights weights,
                             Duration urgency_horizon = std::chrono::seconds(60));

    [[nodiscard]] double score(const Job& job, TimePoint now) const override;
    [[nodiscard]] const char* name() const noexcept override { return "composite"; }

    [[nodiscard]] const Weights& weights() const noexcept { return weights_; }

private:
    Weights weights_;
    Duration urgency_horizon_;
};

}  // namespace chronos
