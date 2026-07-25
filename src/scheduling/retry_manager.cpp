#include "chronos/scheduling/retry_manager.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace chronos {

RetryManager::RetryManager(RetryConfig config) : config_(config), rng_(config.seed) {
    if (config_.multiplier < 1.0) {
        throw std::invalid_argument("RetryConfig: multiplier must be >= 1.0");
    }
    if (config_.jitter < 0.0 || config_.jitter >= 1.0) {
        throw std::invalid_argument("RetryConfig: jitter must be in [0, 1)");
    }
    if (config_.base_backoff < Duration::zero() || config_.max_backoff < Duration::zero()) {
        throw std::invalid_argument("RetryConfig: backoffs must be non-negative");
    }
}

Duration RetryManager::base_delay_for_attempt(int attempt) const {
    const double base_ns = static_cast<double>(config_.base_backoff.count());
    const double max_ns = static_cast<double>(config_.max_backoff.count());
    // Growth in double space, clamped before it can overflow anything.
    const double grown =
        base_ns * std::pow(config_.multiplier, static_cast<double>(attempt - 1));
    const double capped = std::min(grown, max_ns);
    return Duration{static_cast<Duration::rep>(capped)};
}

RetryDecision RetryManager::decide(const Job& job, TimePoint now) {
    // attempt = runs started; max_retries = additional runs after the first.
    if (job.attempt > job.spec.max_retries) {
        return RetryDecision{.action = RetryAction::GiveUp};
    }

    Duration delay = base_delay_for_attempt(job.attempt);

    if (config_.jitter > 0.0) {
        std::uniform_real_distribution<double> dist(1.0 - config_.jitter,
                                                    1.0 + config_.jitter);
        const double scaled = static_cast<double>(delay.count()) * dist(rng_);
        delay = Duration{static_cast<Duration::rep>(scaled)};
        // Jitter may nudge past the ceiling; the ceiling wins.
        delay = std::min(delay, config_.max_backoff);
    }

    return RetryDecision{.action = RetryAction::Retry,
                         .delay = delay,
                         .eligible_at = now + delay};
}

void RetryManager::schedule(JobId id, TimePoint eligible_at) {
    queue_.push({eligible_at, id});
}

std::vector<JobId> RetryManager::collect_due(TimePoint now) {
    std::vector<JobId> due;
    while (!queue_.empty() && queue_.top().eligible_at <= now) {
        due.push_back(queue_.top().id);
        queue_.pop();
    }
    return due;
}

std::optional<TimePoint> RetryManager::next_due() const {
    if (queue_.empty()) {
        return std::nullopt;
    }
    return queue_.top().eligible_at;
}

}  // namespace chronos
