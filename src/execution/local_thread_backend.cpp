#include "chronos/execution/local_thread_backend.h"

#include <string>
#include <utility>

#include "chronos/observability/log.h"

namespace chronos {

LocalThreadBackend::LocalThreadBackend(WorkerRegistry& registry,
                                       SchedulerClient& scheduler, Clock& clock,
                                       JobExecutor executor,
                                       std::vector<ResourceCapacity> capacities,
                                       Duration heartbeat_interval)
    : registry_(registry),
      scheduler_(scheduler),
      clock_(clock),
      executor_(std::move(executor)),
      heartbeat_interval_(heartbeat_interval) {
    slots_.reserve(capacities.size());
    for (std::size_t i = 0; i < capacities.size(); ++i) {
        auto slot = std::make_unique<WorkerSlot>();
        slot->id = registry_.register_worker("local-worker-" + std::to_string(i + 1),
                                             capacities[i]);
        slots_.push_back(std::move(slot));
    }
}

LocalThreadBackend::~LocalThreadBackend() {
    stop();
}

void LocalThreadBackend::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    for (auto& slot : slots_) {
        slot->thread = std::thread(&LocalThreadBackend::worker_loop, this,
                                   std::ref(*slot));
    }
    heartbeat_thread_ = std::thread(&LocalThreadBackend::heartbeat_loop, this);
}

void LocalThreadBackend::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    heartbeat_cv_.notify_all();
    for (auto& slot : slots_) {
        slot->cv.notify_all();
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    for (auto& slot : slots_) {
        if (slot->thread.joinable()) {
            slot->thread.join();
        }
    }
}

void LocalThreadBackend::dispatch(JobAssignment assignment) {
    for (auto& slot : slots_) {
        if (slot->id != assignment.worker_id) {
            continue;
        }
        {
            std::lock_guard lock(slot->mutex);
            slot->inbox.push_back(std::move(assignment));
        }
        slot->cv.notify_one();
        return;
    }
    // Unknown worker: drop silently. The scheduler's heartbeat monitor
    // owns recovery; the transport never invents state.
    log::warn("dispatch to unknown worker dropped",
              {{"worker_id", assignment.worker_id.value()},
               {"job_id", assignment.job_id.value()}});
}

bool LocalThreadBackend::kill_worker(WorkerId id) {
    for (auto& slot : slots_) {
        if (slot->id == id) {
            slot->killed.store(true, std::memory_order_relaxed);
            slot->cv.notify_all();
            log::warn("worker killed (simulated crash)", {{"worker_id", id.value()}});
            return true;
        }
    }
    return false;
}

std::vector<WorkerId> LocalThreadBackend::worker_ids() const {
    std::vector<WorkerId> ids;
    ids.reserve(slots_.size());
    for (const auto& slot : slots_) {
        ids.push_back(slot->id);
    }
    return ids;
}

void LocalThreadBackend::worker_loop(WorkerSlot& slot) {
    while (true) {
        JobAssignment assignment;
        {
            std::unique_lock lock(slot.mutex);
            slot.cv.wait(lock, [&] {
                return !running_.load(std::memory_order_relaxed) ||
                       slot.killed.load(std::memory_order_relaxed) ||
                       !slot.inbox.empty();
            });
            if (slot.killed.load(std::memory_order_relaxed) ||
                !running_.load(std::memory_order_relaxed)) {
                return;  // Crash or shutdown: abandon anything queued.
            }
            assignment = std::move(slot.inbox.front());
            slot.inbox.pop_front();
        }

        scheduler_.report_started(slot.id, assignment.job_id);
        ExecutionResult result = executor_(assignment.job);

        if (slot.killed.load(std::memory_order_relaxed)) {
            return;  // Died mid-job: no completion report, ever.
        }
        scheduler_.report_completion(slot.id, assignment.job_id, std::move(result));
    }
}

void LocalThreadBackend::heartbeat_loop() {
    std::unique_lock lock(heartbeat_mutex_);
    while (running_.load(std::memory_order_relaxed)) {
        for (auto& slot : slots_) {
            if (!slot->killed.load(std::memory_order_relaxed)) {
                scheduler_.report_heartbeat(slot->id);
            }
        }
        heartbeat_cv_.wait_for(lock, heartbeat_interval_, [this] {
            return !running_.load(std::memory_order_relaxed);
        });
    }
}

}  // namespace chronos
