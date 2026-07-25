#pragma once

#include <string>

#include "chronos/core/ids.h"
#include "chronos/core/job.h"

namespace chronos {

/// A scheduling decision: run this job on that worker. Carries a snapshot
/// of the job so the transport needs no access to the JobStore -- exactly
/// what a serialized gRPC message will carry later. The snapshot's
/// `attempt` field is the 1-based number of the run this assignment
/// represents (1 = first execution, 2 = first retry or post-rescue rerun).
struct JobAssignment {
    JobId job_id;
    WorkerId worker_id;
    Job job;  ///< Snapshot at dispatch time.
};

struct ExecutionResult {
    bool success = false;
    std::string error;  ///< Diagnostic on failure.
};

/// The upward half of the seam: how execution reports back to scheduling.
/// The Scheduler implements this; transports call it from their threads
/// (implementations must therefore be thread-safe).
class SchedulerClient {
public:
    virtual ~SchedulerClient() = default;

    /// The worker has begun executing the job (Dispatched -> Running).
    virtual void report_started(WorkerId worker, JobId job) = 0;

    /// The job finished (successfully or not) on the worker.
    virtual void report_completion(WorkerId worker, JobId job, ExecutionResult result) = 0;

    /// Liveness beat from a worker.
    virtual void report_heartbeat(WorkerId worker) = 0;
};

/// The downward half of the seam: how scheduling hands work to execution.
///
/// This is the distributed boundary (ADR-006). The scheduler sees exactly
/// one operation -- "deliver this assignment to that worker's inbox" --
/// and never touches threads, processes, or sockets. LocalThreadBackend
/// implements the inbox as an in-process queue; a future GrpcBackend
/// implements it as a per-worker stream. Nothing above this line changes.
class ExecutionBackend {
public:
    virtual ~ExecutionBackend() = default;

    /// Deliver `assignment` to the target worker. Must not block on job
    /// execution. If the worker is unable to accept (e.g. died between
    /// find_fit and dispatch), the backend simply never reports the job
    /// started -- the heartbeat monitor rescues it.
    virtual void dispatch(JobAssignment assignment) = 0;

    virtual void start() = 0;

    /// Stop accepting and delivering work and release transport resources.
    /// Draining (waiting for in-flight jobs) is the *scheduler's* job --
    /// it owns job state; the transport just closes shop.
    virtual void stop() = 0;
};

}  // namespace chronos
