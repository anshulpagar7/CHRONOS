#include "chronos/core/job_store.h"

#include <string>
#include <utility>

namespace chronos {

namespace {

std::string make_illegal_transition_message(JobId id, JobState from, JobState to) {
    std::string msg = "illegal transition for job ";
    msg += std::to_string(id.value());
    msg += ": ";
    msg += to_string(from);
    msg += " -> ";
    msg += to_string(to);
    return msg;
}

}  // namespace

IllegalTransitionError::IllegalTransitionError(JobId id, JobState from, JobState to)
    : std::logic_error(make_illegal_transition_message(id, from, to)),
      job_id_(id),
      from_(from),
      to_(to) {}

UnknownJobError::UnknownJobError(JobId id)
    : std::out_of_range("unknown job id " + std::to_string(id.value())),
      job_id_(id) {}

JobStore::JobStore(Clock& clock, EventBus& bus) : clock_(clock), bus_(bus) {}

// ---------------------------------------------------------------------------
// The state machine. Kept as one pure function so the entire lifecycle is
// auditable at a glance and trivially unit-tested.
// ---------------------------------------------------------------------------
bool JobStore::transition_allowed(JobState from, JobState to) noexcept {
    if (from == to) {
        return false;  // Self-transitions are always no-ops, hence bugs.
    }
    if (is_terminal(from)) {
        return false;  // Nothing leaves a terminal state.
    }
    if (to == JobState::Cancelled) {
        return true;   // Any non-terminal job may be cancelled.
    }
    switch (from) {
        case JobState::Submitted:
            return to == JobState::Queued;
        case JobState::Queued:
            return to == JobState::Dispatched;
        case JobState::Dispatched:
            // -> Running: worker began execution.
            // -> Queued:  worker rejected the assignment or died before
            //             starting; the attempt was never consumed.
            return to == JobState::Running || to == JobState::Queued;
        case JobState::Running:
            // -> Completed: success.
            // -> RetryWait: retryable failure, backoff pending.
            // -> Failed:    retries exhausted or fatal error.
            // -> Queued:    worker died mid-execution; job rescued by the
            //               heartbeat monitor and rescheduled.
            return to == JobState::Completed || to == JobState::RetryWait ||
                   to == JobState::Failed || to == JobState::Queued;
        case JobState::RetryWait:
            return to == JobState::Queued;
        case JobState::Completed:
        case JobState::Failed:
        case JobState::Cancelled:
            return false;  // Unreachable: is_terminal handled above.
    }
    return false;
}

JobId JobStore::submit(JobSpec spec) {
    Event event{};
    JobId id;
    {
        std::lock_guard lock(mutex_);
        id = JobId{next_job_id_++};

        Job job;
        job.id = id;
        job.spec = std::move(spec);
        job.state = JobState::Submitted;
        job.submit_time = clock_.now();
        job.next_eligible_time = job.submit_time;

        event.type = EventType::JobSubmitted;
        event.timestamp = job.submit_time;
        event.job_id = id;
        event.detail = job.spec.name;

        jobs_.emplace(id, std::move(job));
    }
    // Publish outside the lock: subscribers may call back into the store.
    bus_.publish(event);
    return id;
}

void JobStore::transition(JobId id, JobState to) {
    Event event{};
    {
        std::lock_guard lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) {
            throw UnknownJobError(id);
        }
        Job& job = it->second;

        if (!transition_allowed(job.state, to)) {
            throw IllegalTransitionError(id, job.state, to);
        }

        const JobState from = job.state;
        const TimePoint now = clock_.now();

        job.state = to;
        job.history.push_back({from, to, now});
        if (to == JobState::Running) {
            ++job.attempt;
        }

        event.type = EventType::JobStateChanged;
        event.timestamp = now;
        event.job_id = id;
        event.from_state = from;
        event.to_state = to;
        event.attempt = job.attempt;
    }
    bus_.publish(event);
}

void JobStore::set_next_eligible_time(JobId id, TimePoint at) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        throw UnknownJobError(id);
    }
    it->second.next_eligible_time = at;
}

std::optional<Job> JobStore::get(JobId id) const {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return std::nullopt;
    }
    return it->second;  // Copy: callers get a snapshot, never a live ref.
}

std::size_t JobStore::count(JobState state) const {
    std::lock_guard lock(mutex_);
    std::size_t n = 0;
    for (const auto& [id, job] : jobs_) {
        if (job.state == state) {
            ++n;
        }
    }
    return n;
}

std::size_t JobStore::size() const {
    std::lock_guard lock(mutex_);
    return jobs_.size();
}

std::vector<Job> JobStore::snapshot_in_state(JobState state) const {
    std::lock_guard lock(mutex_);
    std::vector<Job> out;
    for (const auto& [id, job] : jobs_) {
        if (job.state == state) {
            out.push_back(job);
        }
    }
    return out;
}

std::vector<Job> JobStore::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<Job> out;
    out.reserve(jobs_.size());
    for (const auto& [id, job] : jobs_) {
        out.push_back(job);
    }
    return out;
}

}  // namespace chronos
