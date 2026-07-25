#include "chronos/scheduling/policies.h"

#include <limits>

namespace chronos {

namespace {

[[nodiscard]] double to_seconds(Duration d) noexcept {
    return std::chrono::duration<double>(d).count();
}

}  // namespace

double FifoPolicy::score(const Job& job, TimePoint /*now*/) const {
    // Earlier submit -> larger score. Now-independent by construction.
    return -static_cast<double>(job.submit_time.time_since_epoch().count());
}

double PriorityPolicy::score(const Job& job, TimePoint /*now*/) const {
    return static_cast<double>(job.spec.priority);
}

double EdfPolicy::score(const Job& job, TimePoint /*now*/) const {
    if (!job.spec.deadline.has_value()) {
        return -std::numeric_limits<double>::infinity();
    }
    // Earlier deadline -> larger score; overdue jobs rank highest for free.
    return -static_cast<double>(job.spec.deadline->time_since_epoch().count());
}

CompositePolicy::CompositePolicy() : CompositePolicy(Weights{}) {}

CompositePolicy::CompositePolicy(Weights weights, Duration urgency_horizon)
    : weights_(weights), urgency_horizon_(urgency_horizon) {}

double CompositePolicy::score(const Job& job, TimePoint now) const {
    double score = weights_.priority * static_cast<double>(job.spec.priority);

    if (job.spec.deadline.has_value()) {
        const double horizon = to_seconds(urgency_horizon_);
        const double time_left = to_seconds(*job.spec.deadline - now);
        // Bounded urgency in (0, 1]: -> 1 near/past the deadline.
        const double urgency = horizon / (horizon + std::max(time_left, 0.0));
        score += weights_.urgency * urgency;
    }

    const double age_sec = std::max(to_seconds(now - job.submit_time), 0.0);
    score += weights_.aging_per_sec * age_sec;

    score -= weights_.retry_penalty * static_cast<double>(job.attempt);

    return score;
}

}  // namespace chronos
