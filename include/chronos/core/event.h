#pragma once

#include <string>

#include "chronos/core/clock.h"
#include "chronos/core/ids.h"
#include "chronos/core/job.h"

namespace chronos {

/// Every observable occurrence in the system. Extended as components land
/// (WorkerRegistered, HeartbeatMissed, JobDispatched, ... in later phases).
enum class EventType : std::uint8_t {
    JobSubmitted,
    JobStateChanged,
    WorkerRegistered,
    WorkerMarkedDead,
};

[[nodiscard]] const char* to_string(EventType type) noexcept;

/// A single structured event.
///
/// Flat struct rather than a variant hierarchy: cheap to copy, trivial to
/// serialize (logs, /api/events, timeline), and fields simply go unused when
/// irrelevant to a given type. Revisit if payloads diverge significantly.
struct Event {
    EventType type;
    TimePoint timestamp{};

    JobId job_id{};
    WorkerId worker_id{};

    // For JobStateChanged.
    JobState from_state = JobState::Submitted;
    JobState to_state   = JobState::Submitted;
    int attempt = 0;

    std::string detail;  ///< Optional human-readable context.
};

}  // namespace chronos
