#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "chronos/core/clock.h"
#include "chronos/core/event_bus.h"
#include "chronos/core/job.h"

namespace chronos {

/// Thrown when a transition violates the job state machine.
class IllegalTransitionError : public std::logic_error {
public:
    IllegalTransitionError(JobId id, JobState from, JobState to);
    [[nodiscard]] JobId job_id() const noexcept { return job_id_; }
    [[nodiscard]] JobState from() const noexcept { return from_; }
    [[nodiscard]] JobState to() const noexcept { return to_; }

private:
    JobId job_id_;
    JobState from_;
    JobState to_;
};

/// Thrown when an operation references a job id the store has never seen.
class UnknownJobError : public std::out_of_range {
public:
    explicit UnknownJobError(JobId id);
    [[nodiscard]] JobId job_id() const noexcept { return job_id_; }

private:
    JobId job_id_;
};

/// The single source of truth for every Job in the system.
///
/// Design rules (see docs/decisions/ADR-003-job-state-machine.md):
///  * The store owns the canonical Job objects. Everything else -- the
///    scheduler's ready set, workers, the API layer -- holds JobIds or
///    snapshot copies, never live references. This kills the
///    "three divergent copies of a job" bug class from v1 at the root.
///  * Every state change goes through transition(), which validates the
///    move against a static transition table, timestamps it, appends it
///    to the job's history, and emits a JobStateChanged event. Illegal
///    transitions throw -- a scheduler bug surfaces as a loud exception,
///    never as silent state corruption.
///
/// Thread-safe. Events are published *after* the internal lock is released,
/// so subscribers may safely call back into the store.
class JobStore {
public:
    JobStore(Clock& clock, EventBus& bus);

    JobStore(const JobStore&) = delete;
    JobStore& operator=(const JobStore&) = delete;

    /// Register a new job in state Submitted. Assigns its id, stamps
    /// submit_time, and emits JobSubmitted. Returns the new id.
    JobId submit(JobSpec spec);

    /// Move a job to `to`, enforcing the state machine.
    /// Entering Running increments the job's attempt counter.
    /// Throws UnknownJobError / IllegalTransitionError.
    void transition(JobId id, JobState to);

    /// Set the earliest time a job may be rescheduled (retry backoff).
    void set_next_eligible_time(JobId id, TimePoint at);

    /// Snapshot of a single job, or nullopt if unknown.
    [[nodiscard]] std::optional<Job> get(JobId id) const;

    /// Number of jobs currently in `state`.
    [[nodiscard]] std::size_t count(JobState state) const;

    /// Total number of jobs ever submitted (all states).
    [[nodiscard]] std::size_t size() const;

    /// Snapshot of every job. Intended for the API layer and tests,
    /// not hot paths.
    [[nodiscard]] std::vector<Job> snapshot() const;

    /// Snapshot of only the jobs currently in `state`. O(total jobs) scan
    /// but copies only the matching subset -- the right tool for hot-ish
    /// paths like rescoring (Queued) and retry scanning (RetryWait), where
    /// copying every terminal job's history would dominate the cost.
    [[nodiscard]] std::vector<Job> snapshot_in_state(JobState state) const;

    /// Whether the state machine permits `from` -> `to`. Pure; exposed
    /// so schedulers can pre-check without try/catch control flow.
    [[nodiscard]] static bool transition_allowed(JobState from, JobState to) noexcept;

private:
    Clock& clock_;
    EventBus& bus_;

    mutable std::mutex mutex_;
    std::unordered_map<JobId, Job> jobs_;
    std::uint64_t next_job_id_ = 1;
};

}  // namespace chronos
