#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "chronos/core/clock.h"
#include "chronos/core/ids.h"

namespace chronos {

/// Lifecycle states of a job.
///
///   Submitted -> Queued -> Dispatched -> Running -> Completed
///                  ^            |           |
///                  |            |           +--> RetryWait --> Queued (backoff loop)
///                  |            |           +--> Failed        (retries exhausted / fatal)
///                  +------------+  (worker rejected / died before start)
///
/// Cancelled is reachable from any non-terminal state.
/// Terminal states: Completed, Failed, Cancelled.
/// The full transition table lives in JobStore and is enforced at runtime.
enum class JobState : std::uint8_t {
    Submitted,   ///< Accepted by the system, not yet visible to the scheduler.
    Queued,      ///< In the ready set, eligible for scheduling.
    Dispatched,  ///< Assigned to a worker, not yet executing.
    Running,     ///< Executing on a worker.
    RetryWait,   ///< Failed; waiting out exponential backoff before re-queue.
    Completed,   ///< Terminal: finished successfully.
    Failed,      ///< Terminal: retries exhausted or non-retryable failure.
    Cancelled,   ///< Terminal: cancelled by the user.
};

[[nodiscard]] const char* to_string(JobState state) noexcept;
[[nodiscard]] bool is_terminal(JobState state) noexcept;

/// Resources a job needs to execute. The scheduler will only dispatch a job
/// to a worker with at least this much free capacity.
struct ResourceRequest {
    std::uint32_t cpu_units = 1;
    std::uint32_t memory_mb = 64;
};

/// Everything the *submitter* specifies about a job.
/// Identity, state, and timestamps are owned by the system (see Job).
struct JobSpec {
    std::string name = "job";
    int priority = 0;                       ///< Higher = more important.
    std::optional<TimePoint> deadline;      ///< Absolute deadline, if any.
    int max_retries = 0;                    ///< Additional attempts after the first.
    ResourceRequest resources{};
    std::string payload;                    ///< Opaque to the scheduler.
};

/// Fluent builder with validation, so an invalid JobSpec can never enter
/// the system. Usage:
///
///   auto spec = JobSpecBuilder{}
///                   .name("encode-video")
///                   .priority(5)
///                   .max_retries(3)
///                   .resources({.cpu_units = 2, .memory_mb = 512})
///                   .build();
class JobSpecBuilder {
public:
    JobSpecBuilder& name(std::string value) {
        spec_.name = std::move(value);
        return *this;
    }
    /// Priority domain is [0, 9] (validated in build()). The bound is
    /// load-bearing: CompositePolicy's weights are balanced for it, so an
    /// unbounded priority would drown the urgency and aging terms and
    /// defeat the anti-starvation design.
    JobSpecBuilder& priority(int value) {
        spec_.priority = value;
        return *this;
    }
    JobSpecBuilder& deadline(TimePoint value) {
        spec_.deadline = value;
        return *this;
    }
    JobSpecBuilder& max_retries(int value) {
        spec_.max_retries = value;
        return *this;
    }
    JobSpecBuilder& resources(ResourceRequest value) {
        spec_.resources = value;
        return *this;
    }
    JobSpecBuilder& payload(std::string value) {
        spec_.payload = std::move(value);
        return *this;
    }

    [[nodiscard]] JobSpec build() const {
        if (spec_.name.empty()) {
            throw std::invalid_argument("JobSpec: name must not be empty");
        }
        if (spec_.max_retries < 0) {
            throw std::invalid_argument("JobSpec: max_retries must be >= 0");
        }
        if (spec_.priority < 0 || spec_.priority > 9) {
            throw std::invalid_argument("JobSpec: priority must be in [0, 9]");
        }
        if (spec_.resources.cpu_units == 0) {
            throw std::invalid_argument("JobSpec: cpu_units must be >= 1");
        }
        if (spec_.resources.memory_mb == 0) {
            throw std::invalid_argument("JobSpec: memory_mb must be >= 1");
        }
        return spec_;
    }

private:
    JobSpec spec_;
};

/// One entry in a job's execution timeline.
struct StateChange {
    JobState from;
    JobState to;
    TimePoint at;
};

/// The canonical job record, owned exclusively by JobStore.
/// Copies handed out by the store are snapshots, never live references.
struct Job {
    JobId id;
    JobSpec spec;
    JobState state = JobState::Submitted;

    /// Number of times execution has *started* (i.e. entries into Running).
    int attempt = 0;

    TimePoint submit_time{};

    /// Earliest time this job may be scheduled again (set by retry backoff).
    TimePoint next_eligible_time{};

    /// Full transition history -- the seed of the execution-timeline feature.
    std::vector<StateChange> history;
};

}  // namespace chronos
