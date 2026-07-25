#include "chronos/scheduling/scheduler.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "chronos/observability/log.h"

namespace chronos {

Scheduler::Scheduler(JobStore& store, WorkerRegistry& registry, EventBus& bus,
                     Clock& clock, std::unique_ptr<SchedulingPolicy> policy,
                     RetryConfig retry_config, SchedulerConfig config)
    : store_(store),
      registry_(registry),
      bus_(bus),
      clock_(clock),
      policy_(std::move(policy)),
      config_(config),
      ready_(*policy_),
      retries_(retry_config) {
    if (!policy_) {
        throw std::invalid_argument("Scheduler: policy must not be null");
    }
}

Scheduler::~Scheduler() {
    stop(/*drain=*/false);
}

void Scheduler::attach_backend(ExecutionBackend& backend) {
    backend_ = &backend;
}

// ---------------------------------------------------------------------------
// Public API (any thread)
// ---------------------------------------------------------------------------

JobId Scheduler::submit(JobSpec spec) {
    const JobId id = store_.submit(std::move(spec));
    store_.transition(id, JobState::Queued);
    enqueue_command(QueuedCmd{id});
    return id;
}

bool Scheduler::cancel(JobId id) {
    const auto job = store_.get(id);
    if (!job || is_terminal(job->state) || job->state == JobState::Running) {
        return false;  // Best-effort: running jobs finish.
    }
    enqueue_command(CancelCmd{id});
    return true;
}

void Scheduler::report_started(WorkerId worker, JobId job) {
    enqueue_command(StartedCmd{worker, job});
}

void Scheduler::report_completion(WorkerId worker, JobId job, ExecutionResult result) {
    enqueue_command(CompletionCmd{worker, job, std::move(result)});
}

void Scheduler::report_heartbeat(WorkerId worker) {
    registry_.heartbeat(worker);  // Registry is thread-safe; no wake needed.
}

std::size_t Scheduler::live_jobs() const {
    return store_.count(JobState::Submitted) + store_.count(JobState::Queued) +
           store_.count(JobState::Dispatched) + store_.count(JobState::Running) +
           store_.count(JobState::RetryWait);
}

std::optional<std::pair<JobId, WorkerId>> Scheduler::active_reservation() const {
    std::lock_guard lock(reservation_mutex_);
    return reservation_view_;
}

void Scheduler::enqueue_command(Command cmd) {
    {
        std::lock_guard lock(cmd_mutex_);
        commands_.push_back(std::move(cmd));
    }
    cmd_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void Scheduler::start() {
    if (!backend_) {
        throw std::logic_error("Scheduler::start: no backend attached");
    }
    {
        std::lock_guard lock(cmd_mutex_);
        if (running_) {
            return;
        }
        running_ = true;
    }
    thread_ = std::thread(&Scheduler::loop, this);
    log::info("scheduler started", {{"policy", policy_->name()}});
}

void Scheduler::stop(bool drain) {
    if (drain) {
        // The loop keeps scheduling; we merely wait for quiescence.
        std::unique_lock lock(cmd_mutex_);
        drain_cv_.wait(lock, [this] { return !running_ || live_jobs() == 0; });
    }
    {
        std::lock_guard lock(cmd_mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    cmd_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    log::info("scheduler stopped", {{"live_jobs",
                                     static_cast<std::uint64_t>(live_jobs())}});
}

void Scheduler::loop() {
    while (true) {
        run_once(clock_.now());
        drain_cv_.notify_all();  // Let stop(drain) re-check quiescence.

        std::unique_lock lock(cmd_mutex_);
        if (!running_) {
            break;
        }
        if (!commands_.empty()) {
            continue;  // More work arrived while dispatching.
        }
        cmd_cv_.wait_for(lock, next_wait(clock_.now()),
                         [this] { return !running_ || !commands_.empty(); });
        if (!running_) {
            break;
        }
    }
}

Duration Scheduler::next_wait(TimePoint now) const {
    Duration wait = config_.max_idle_wait;
    if (const auto due = retries_.next_due()) {
        wait = std::min(wait, std::max(*due - now, Duration::zero()));
    }
    const Duration until_rescore = (last_rescore_ + config_.rescore_interval) - now;
    wait = std::min(wait, std::max(until_rescore, Duration::zero()));
    return std::max(wait, Duration{std::chrono::milliseconds(1)});
}

// ---------------------------------------------------------------------------
// The scheduling iteration (scheduler thread / tests / simulator)
// ---------------------------------------------------------------------------

void Scheduler::run_once(TimePoint now) {
    process_commands(now);
    requeue_due_retries(now);
    sweep_dead_workers(now);
    maybe_rescore(now);
    dispatch_round(now);
}

void Scheduler::process_commands(TimePoint now) {
    std::deque<Command> batch;
    {
        std::lock_guard lock(cmd_mutex_);
        batch.swap(commands_);
    }
    for (Command& cmd : batch) {
        std::visit(
            [&](auto& c) {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, QueuedCmd>) {
                    handle_queued(c.id, now);
                } else if constexpr (std::is_same_v<T, CancelCmd>) {
                    handle_cancel(c.id);
                } else if constexpr (std::is_same_v<T, StartedCmd>) {
                    handle_started(c);
                } else if constexpr (std::is_same_v<T, CompletionCmd>) {
                    handle_completion(c, now);
                }
            },
            cmd);
    }
}

void Scheduler::handle_queued(JobId id, TimePoint now) {
    const auto job = store_.get(id);
    if (!job || job->state != JobState::Queued) {
        return;  // Cancelled (or otherwise moved on) before we saw it.
    }
    ready_.add(*job, now);
}

void Scheduler::handle_cancel(JobId id) {
    const auto job = store_.get(id);
    if (!job) {
        return;
    }
    try {
        store_.transition(id, JobState::Cancelled);
    } catch (const IllegalTransitionError&) {
        return;  // Raced into Running/terminal; best-effort means we yield.
    }
    ready_.remove(id);
    if (reserved_job_ == id) {
        reserved_job_.reset();
        reserved_worker_.reset();
        publish_reservation_view();
    }
    // If it was Dispatched, resources are still charged; the transport will
    // never report it, and completion accounting happens via in_flight_ --
    // reclaim now.
    if (auto it = in_flight_.find(id); it != in_flight_.end()) {
        registry_.release(it->second.first, it->second.second, id);
        in_flight_.erase(it);
    }
}

void Scheduler::handle_started(const StartedCmd& cmd) {
    try {
        store_.transition(cmd.id, JobState::Running);
    } catch (const IllegalTransitionError&) {
        // Stale: the job was rescued or cancelled before the start report
        // crossed the queue. The transport's execution is now a ghost; its
        // completion will be ignored the same way.
        log::warn("ignoring stale start report",
                  {{"job_id", cmd.id.value()}, {"worker_id", cmd.worker.value()}});
    } catch (const UnknownJobError&) {
        log::warn("start report for unknown job", {{"job_id", cmd.id.value()}});
    }
}

void Scheduler::handle_completion(const CompletionCmd& cmd, TimePoint now) {
    // Release resources exactly once, keyed by our own dispatch record.
    if (auto it = in_flight_.find(cmd.id);
        it != in_flight_.end() && it->second.first == cmd.worker) {
        registry_.release(cmd.worker, it->second.second, cmd.id);
        in_flight_.erase(it);
    }

    const auto job = store_.get(cmd.id);
    if (!job || job->state != JobState::Running) {
        // Rescued/cancelled while executing: the authoritative state has
        // moved on; this report is history.
        log::warn("ignoring stale completion",
                  {{"job_id", cmd.id.value()}, {"worker_id", cmd.worker.value()}});
        return;
    }

    if (cmd.result.success) {
        store_.transition(cmd.id, JobState::Completed);
        return;
    }

    const RetryDecision decision = retries_.decide(*job, now);
    if (decision.action == RetryAction::Retry) {
        store_.transition(cmd.id, JobState::RetryWait);
        store_.set_next_eligible_time(cmd.id, decision.eligible_at);
        retries_.schedule(cmd.id, decision.eligible_at);
        log::info("job scheduled for retry",
                  {{"job_id", cmd.id.value()},
                   {"attempt", job->attempt},
                   {"delay_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                    decision.delay)
                                    .count()},
                   {"error", cmd.result.error}});
    } else {
        store_.transition(cmd.id, JobState::Failed);
        log::warn("job dead-lettered",
                  {{"job_id", cmd.id.value()},
                   {"attempt", job->attempt},
                   {"error", cmd.result.error}});
    }
}

void Scheduler::requeue_due_retries(TimePoint now) {
    for (const JobId id : retries_.collect_due(now)) {
        requeue(id, now);
    }
}

void Scheduler::sweep_dead_workers(TimePoint now) {
    for (const DeadWorker& dead : registry_.collect_dead(config_.heartbeat_timeout)) {
        if (reserved_worker_ == dead.id) {
            reserved_job_.reset();
            reserved_worker_.reset();
            publish_reservation_view();
        }
        for (const JobId id : dead.orphaned_jobs) {
            in_flight_.erase(id);  // Registry already zeroed the account.
            requeue(id, now);
            log::warn("job rescued from dead worker",
                      {{"job_id", id.value()}, {"worker_id", dead.id.value()}});
        }
    }
}

void Scheduler::requeue(JobId id, TimePoint now) {
    const auto job = store_.get(id);
    if (!job) {
        return;
    }
    try {
        store_.transition(id, JobState::Queued);
    } catch (const IllegalTransitionError&) {
        return;  // e.g. cancelled while in RetryWait.
    }
    if (const auto fresh = store_.get(id)) {
        ready_.add(*fresh, now);
    }
}

void Scheduler::maybe_rescore(TimePoint now) {
    if (now - last_rescore_ < config_.rescore_interval && last_rescore_ != TimePoint{}) {
        return;
    }
    last_rescore_ = now;
    if (ready_.empty()) {
        return;
    }
    ready_.rescore(store_.snapshot_in_state(JobState::Queued), now);
}

void Scheduler::clear_reservation_if_stale() {
    if (!reserved_job_) {
        return;
    }
    // The reserved job must still be queued and its worker still alive.
    const auto job = store_.get(*reserved_job_);
    const bool job_gone = !job || job->state != JobState::Queued;
    const auto worker = registry_.get(*reserved_worker_);
    const bool worker_gone = !worker || !worker->alive;
    if (job_gone || worker_gone) {
        reserved_job_.reset();
        reserved_worker_.reset();
        publish_reservation_view();
    }
}

void Scheduler::dispatch_round(TimePoint /*now*/) {
    if (!backend_) {
        return;
    }
    clear_reservation_if_stale();

    while (!ready_.empty()) {
        // A job is eligible if some worker fits it -- honouring the
        // reservation: the reserved worker accepts only the reserved job.
        const auto eligible = [&](JobId id) {
            const auto job = store_.get(id);
            if (!job || job->state != JobState::Queued) {
                return false;
            }
            const std::optional<WorkerId> exclude =
                (reserved_job_ && id != *reserved_job_) ? reserved_worker_
                                                        : std::nullopt;
            return registry_.find_fit(job->spec.resources, exclude).has_value();
        };

        const auto picked = ready_.best_where(eligible, config_.dispatch_scan_bound);
        if (!picked) {
            // Nothing fits. If the head of the queue has been passed over
            // repeatedly, grant it a reservation: freeze the biggest worker
            // that could ever host it until it drains enough (ADR-007).
            if (const auto head = ready_.best();
                head && !reserved_job_ &&
                ready_.skip_count(*head) >= config_.backfill_skip_threshold) {
                if (const auto job = store_.get(*head)) {
                    if (const auto target =
                            registry_.find_reservation_target(job->spec.resources)) {
                        reserved_job_ = *head;
                        reserved_worker_ = *target;
                        publish_reservation_view();
                        log::info("backfill reservation placed",
                                  {{"job_id", head->value()},
                                   {"worker_id", target->value()},
                                   {"skips", ready_.skip_count(*head)}});
                    }
                }
            }
            break;
        }

        const auto job = store_.get(*picked);
        const std::optional<WorkerId> exclude =
            (reserved_job_ && *picked != *reserved_job_) ? reserved_worker_
                                                         : std::nullopt;
        const auto worker = registry_.find_fit(job->spec.resources, exclude);
        if (!worker || !registry_.try_allocate(*worker, job->spec.resources, *picked)) {
            break;  // Raced (e.g. death sweep); next iteration will retry.
        }

        ready_.remove(*picked);
        if (reserved_job_ == *picked) {
            reserved_job_.reset();
            reserved_worker_.reset();
            publish_reservation_view();
        }
        in_flight_[*picked] = {*worker, job->spec.resources};
        store_.transition(*picked, JobState::Dispatched);

        JobAssignment assignment;
        assignment.job_id = *picked;
        assignment.worker_id = *worker;
        assignment.job = *store_.get(*picked);
        // The snapshot represents run N+1: the store's attempt counter (runs
        // *started*) increments only when the StartedCmd lands, so surface
        // the 1-based number of THIS run to the executor.
        assignment.job.attempt += 1;
        backend_->dispatch(std::move(assignment));
    }
}

void Scheduler::publish_reservation_view() {
    std::lock_guard lock(reservation_mutex_);
    if (reserved_job_ && reserved_worker_) {
        reservation_view_ = std::make_pair(*reserved_job_, *reserved_worker_);
    } else {
        reservation_view_.reset();
    }
}

}  // namespace chronos
