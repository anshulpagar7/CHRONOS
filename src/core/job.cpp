#include "chronos/core/job.h"

#include "chronos/core/event.h"

namespace chronos {

const char* to_string(JobState state) noexcept {
    switch (state) {
        case JobState::Submitted:  return "SUBMITTED";
        case JobState::Queued:     return "QUEUED";
        case JobState::Dispatched: return "DISPATCHED";
        case JobState::Running:    return "RUNNING";
        case JobState::RetryWait:  return "RETRY_WAIT";
        case JobState::Completed:  return "COMPLETED";
        case JobState::Failed:     return "FAILED";
        case JobState::Cancelled:  return "CANCELLED";
    }
    return "UNKNOWN";
}

bool is_terminal(JobState state) noexcept {
    return state == JobState::Completed ||
           state == JobState::Failed ||
           state == JobState::Cancelled;
}

const char* to_string(EventType type) noexcept {
    switch (type) {
        case EventType::JobSubmitted:     return "JOB_SUBMITTED";
        case EventType::JobStateChanged:  return "JOB_STATE_CHANGED";
        case EventType::WorkerRegistered: return "WORKER_REGISTERED";
        case EventType::WorkerMarkedDead: return "WORKER_MARKED_DEAD";
    }
    return "UNKNOWN";
}

}  // namespace chronos
