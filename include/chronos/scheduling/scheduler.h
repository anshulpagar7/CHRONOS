#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>

#include "chronos/core/clock.h"
#include "chronos/core/event_bus.h"
#include "chronos/core/job_store.h"
#include "chronos/execution/execution_backend.h"
#include "chronos/execution/worker_registry.h"
#include "chronos/scheduling/ready_set.h"
#include "chronos/scheduling/retry_manager.h"
#include "chronos/scheduling/scheduling_policy.h"

namespace chronos {

struct SchedulerConfig {
    /// How often queued jobs are re-scored (makes aging real, ADR-004).
    Duration rescore_interval = std::chrono::milliseconds(500);
    /// A worker silent this long is dead (jobs rescued, ADR-007).
    Duration heartbeat_timeout = std::chrono::seconds(3);
    /// best_where() scan bound per dispatch round.
    std::size_t dispatch_scan_bound = 64;
    /// Skips before the top job earns a capacity reservation (backfill).
    int backfill_skip_threshold = 3;
    /// Upper bound on one loop iteration's sleep; keeps periodic duties
    /// (heartbeat sweeps, rescoring) timely even with nothing due.
    Duration max_idle_wait = std::chrono::milliseconds(50);
};

/// The conductor. One thread owns all scheduling state (ReadySet,
/// RetryManager, the backfill reservation); everything else talks to it
/// through a command queue -- an actor, in effect (ADR-006).
///
///   submit()/cancel()  (any thread)  --> command queue --> scheduler loop
///   SchedulerClient    (transport threads) --> command queue
///   scheduler loop --> ExecutionBackend::dispatch --> worker inboxes
///
/// The loop, each wake: drain commands -> requeue ripe retries -> sweep
/// dead workers & rescue orphans -> rescore on tick -> dispatch until no
/// eligible (job, worker) pair remains -> sleep until the next deadline
/// (retry ripening, rescore tick, or heartbeat sweep) or a notify.
///
/// All per-iteration logic lives in run_once(now), public so unit tests
/// and the Phase-4 simulator drive the *real* scheduler deterministically
/// on a SimulatedClock -- no thread, no sleeps.
class Scheduler final : public SchedulerClient {
public:
    Scheduler(JobStore& store, WorkerRegistry& registry, EventBus& bus, Clock& clock,
              std::unique_ptr<SchedulingPolicy> policy, RetryConfig retry_config = {},
              SchedulerConfig config = {});
    ~Scheduler() override;

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// Must be called (once) before start(); split from the constructor
    /// because the backend needs a SchedulerClient& (this) to be built.
    void attach_backend(ExecutionBackend& backend);

    /// Accept a job (thread-safe; valid before start and while running).
    JobId submit(JobSpec spec);

    /// Best-effort cancel: succeeds for jobs not yet running (Submitted /
    /// Queued / Dispatched / RetryWait). Running jobs finish; terminal
    /// jobs are unaffected. Returns whether cancellation was accepted.
    bool cancel(JobId id);

    // SchedulerClient (called from transport threads).
    void report_started(WorkerId worker, JobId job) override;
    void report_completion(WorkerId worker, JobId job, ExecutionResult result) override;
    void report_heartbeat(WorkerId worker) override;

    /// Start the scheduling thread.
    void start();

    /// Stop scheduling. drain=true first waits until no job is live
    /// (Queued/Dispatched/Running/RetryWait); drain=false stops after the
    /// current iteration, leaving unfinished jobs in place.
    void stop(bool drain = true);

    /// Jobs not yet in a terminal state.
    [[nodiscard]] std::size_t live_jobs() const;

    /// One full scheduling iteration at `now`. Exposed for deterministic
    /// tests and the simulator; the background thread calls exactly this.
    void run_once(TimePoint now);

    /// Currently reserved (job, worker), if a backfill reservation is
    /// active. Exposed for tests and the API layer.
    [[nodiscard]] std::optional<std::pair<JobId, WorkerId>> active_reservation() const;

private:
    // -- Commands crossing into the scheduler thread ----------------------
    struct QueuedCmd { JobId id; };
    struct CancelCmd { JobId id; };
    struct StartedCmd { WorkerId worker; JobId id; };
    struct CompletionCmd { WorkerId worker; JobId id; ExecutionResult result; };
    using Command = std::variant<QueuedCmd, CancelCmd, StartedCmd, CompletionCmd>;

    void enqueue_command(Command cmd);

    // -- Loop stages (scheduler thread only) -------------------------------
    void process_commands(TimePoint now);
    void handle_queued(JobId id, TimePoint now);
    void handle_cancel(JobId id);
    void handle_started(const StartedCmd& cmd);
    void handle_completion(const CompletionCmd& cmd, TimePoint now);
    void requeue_due_retries(TimePoint now);
    void sweep_dead_workers(TimePoint now);
    void maybe_rescore(TimePoint now);
    void dispatch_round(TimePoint now);
    void clear_reservation_if_stale();

    /// Requeue a job (rescue / retry-ripened): transition + ReadySet add.
    void requeue(JobId id, TimePoint now);

    [[nodiscard]] Duration next_wait(TimePoint now) const;

    void publish_reservation_view();

    void loop();

    // -- Collaborators ------------------------------------------------------
    JobStore& store_;
    WorkerRegistry& registry_;
    EventBus& bus_;
    Clock& clock_;
    ExecutionBackend* backend_ = nullptr;

    std::unique_ptr<SchedulingPolicy> policy_;
    SchedulerConfig config_;

    // -- Scheduler-thread-owned state (no locks; see ADR-004/005) ----------
    ReadySet ready_;
    RetryManager retries_;
    TimePoint last_rescore_{};
    std::optional<JobId> reserved_job_;
    std::optional<WorkerId> reserved_worker_;
    /// Resources charged per in-flight job (for release on completion).
    std::unordered_map<JobId, std::pair<WorkerId, ResourceRequest>> in_flight_;

    // -- Cross-thread machinery --------------------------------------------
    mutable std::mutex cmd_mutex_;
    std::condition_variable cmd_cv_;
    std::condition_variable drain_cv_;
    std::deque<Command> commands_;
    bool running_ = false;
    std::thread thread_;

    // Reservation mirror readable from any thread.
    mutable std::mutex reservation_mutex_;
    std::optional<std::pair<JobId, WorkerId>> reservation_view_;
};

}  // namespace chronos
