#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chronos/core/clock.h"
#include "chronos/core/event_bus.h"
#include "chronos/core/ids.h"
#include "chronos/core/job.h"

namespace chronos {

using ResourceCapacity = ResourceRequest;

/// Everything the scheduler knows about one worker. Note what is absent:
/// threads, sockets, processes. A worker is a *record* -- id, capacity,
/// liveness, running jobs. Transports (local threads today, gRPC later)
/// maintain these records identically, which is what makes the execution
/// backend swappable (ADR-006).
struct WorkerInfo {
    WorkerId id;
    std::string name;
    ResourceCapacity total{};
    ResourceCapacity available{};
    TimePoint last_heartbeat{};
    bool alive = true;
    std::unordered_set<JobId> running_jobs;
};

/// A dead worker together with the jobs it orphaned (for rescue).
struct DeadWorker {
    WorkerId id;
    std::vector<JobId> orphaned_jobs;
};

/// Thread-safe registry of workers and their resource accounts.
///
/// Called from the scheduler thread (allocation, dead-worker sweeps) and
/// from transport threads (heartbeats), hence internally synchronized --
/// unlike ReadySet/RetryManager, which are scheduler-thread-only.
///
/// Liveness is lease-based: workers heartbeat; a worker whose last beat is
/// older than the timeout is marked dead by collect_dead(), its resources
/// are freed, and its in-flight jobs are handed back for rescheduling.
/// Death is permanent: a revived transport must re-register as a new
/// worker. This avoids the classic split-brain of a "resurrected" worker
/// still executing a job the scheduler already handed to someone else.
class WorkerRegistry {
public:
    WorkerRegistry(Clock& clock, EventBus& bus);

    WorkerRegistry(const WorkerRegistry&) = delete;
    WorkerRegistry& operator=(const WorkerRegistry&) = delete;

    /// Add a worker with the given capacity; the birth heartbeat is now.
    /// Emits WorkerRegistered.
    WorkerId register_worker(std::string name, ResourceCapacity capacity);

    /// Record a liveness beat. Beats from dead/unknown workers are ignored
    /// (returns false).
    bool heartbeat(WorkerId id);

    /// Atomically check-and-reserve `req` on worker `id` for `job`.
    /// Fails (false) if the worker is unknown, dead, already running the
    /// job, or lacks capacity.
    bool try_allocate(WorkerId id, const ResourceRequest& req, JobId job);

    /// Return `job`'s resources on worker `id`. No-op (false) if the
    /// worker is unknown/dead or wasn't running the job -- releases after
    /// a death sweep must be harmless (the sweep already freed everything).
    bool release(WorkerId id, const ResourceRequest& req, JobId job);

    /// Alive worker with enough free capacity for `req`, preferring the
    /// most free CPU (load balancing), ties to the lowest id
    /// (determinism). `exclude` supports backfill reservations.
    [[nodiscard]] std::optional<WorkerId> find_fit(
        const ResourceRequest& req,
        std::optional<WorkerId> exclude = std::nullopt) const;

    /// Alive worker whose *total* capacity could ever satisfy `req`
    /// (regardless of current load): the backfill reservation target.
    /// Prefers the largest total CPU, ties to the lowest id.
    [[nodiscard]] std::optional<WorkerId> find_reservation_target(
        const ResourceRequest& req) const;

    /// Mark every worker silent for longer than `timeout` as dead, free its
    /// resources, and return the orphaned jobs per worker. Emits
    /// WorkerMarkedDead. Call from the scheduler loop.
    std::vector<DeadWorker> collect_dead(Duration timeout);

    [[nodiscard]] std::optional<WorkerInfo> get(WorkerId id) const;
    [[nodiscard]] std::vector<WorkerInfo> snapshot() const;
    [[nodiscard]] std::size_t alive_count() const;

private:
    Clock& clock_;
    EventBus& bus_;

    mutable std::mutex mutex_;
    std::unordered_map<WorkerId, WorkerInfo> workers_;
    std::uint64_t next_worker_id_ = 1;
};

}  // namespace chronos
