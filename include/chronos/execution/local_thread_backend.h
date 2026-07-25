#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chronos/core/clock.h"
#include "chronos/execution/execution_backend.h"
#include "chronos/execution/worker_registry.h"

namespace chronos {

/// What a worker actually does with a job. Injected so tests execute
/// instantly and deterministically, demos simulate work, and real
/// workloads plug in later without touching the transport.
using JobExecutor = std::function<ExecutionResult(const Job&)>;

/// The first ExecutionBackend: workers as threads in this process.
///
/// Faithful to the distributed shape (ADR-006):
///  * each worker has an *inbox* (mutex + cv + deque) that dispatch()
///    pushes into -- the in-process stand-in for a per-worker gRPC stream;
///  * workers report started/completed through SchedulerClient, exactly
///    as a remote worker would over the wire;
///  * liveness is a *sidecar heartbeat thread* beating for every living
///    worker, so a worker busy executing a long job still reads as alive
///    (a per-worker beat inside the run loop would false-positive the
///    death detector during any job longer than the timeout).
///
/// kill_worker(id) simulates a crash: the worker stops heartbeating and
/// never reports its in-flight job. Nothing tells the scheduler -- that is
/// the point. The heartbeat monitor must notice and rescue. Used by the
/// chaos tests and the day-3 demo.
class LocalThreadBackend final : public ExecutionBackend {
public:
    /// Registers one worker per capacity entry into `registry`.
    LocalThreadBackend(WorkerRegistry& registry, SchedulerClient& scheduler,
                       Clock& clock, JobExecutor executor,
                       std::vector<ResourceCapacity> capacities,
                       Duration heartbeat_interval = std::chrono::milliseconds(50));
    ~LocalThreadBackend() override;

    LocalThreadBackend(const LocalThreadBackend&) = delete;
    LocalThreadBackend& operator=(const LocalThreadBackend&) = delete;

    void dispatch(JobAssignment assignment) override;
    void start() override;
    void stop() override;

    /// Simulate a hard crash of `id`: heartbeats cease, the in-flight job
    /// (if any) is silently abandoned. Returns false for unknown ids.
    bool kill_worker(WorkerId id);

    [[nodiscard]] std::vector<WorkerId> worker_ids() const;

private:
    struct WorkerSlot {
        WorkerId id;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<JobAssignment> inbox;
        std::atomic<bool> killed{false};
        std::thread thread;
    };

    void worker_loop(WorkerSlot& slot);
    void heartbeat_loop();

    WorkerRegistry& registry_;
    SchedulerClient& scheduler_;
    Clock& clock_;
    JobExecutor executor_;
    Duration heartbeat_interval_;

    std::vector<std::unique_ptr<WorkerSlot>> slots_;

    std::atomic<bool> running_{false};
    std::thread heartbeat_thread_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;
};

}  // namespace chronos
