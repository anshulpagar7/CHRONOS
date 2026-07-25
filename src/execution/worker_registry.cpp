#include "chronos/execution/worker_registry.h"

#include <utility>

namespace chronos {

WorkerRegistry::WorkerRegistry(Clock& clock, EventBus& bus) : clock_(clock), bus_(bus) {}

WorkerId WorkerRegistry::register_worker(std::string name, ResourceCapacity capacity) {
    Event event{};
    WorkerId id;
    {
        std::lock_guard lock(mutex_);
        id = WorkerId{next_worker_id_++};

        WorkerInfo info;
        info.id = id;
        info.name = std::move(name);
        info.total = capacity;
        info.available = capacity;
        info.last_heartbeat = clock_.now();

        event.type = EventType::WorkerRegistered;
        event.timestamp = info.last_heartbeat;
        event.worker_id = id;
        event.detail = info.name;

        workers_.emplace(id, std::move(info));
    }
    bus_.publish(event);
    return id;
}

bool WorkerRegistry::heartbeat(WorkerId id) {
    std::lock_guard lock(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end() || !it->second.alive) {
        return false;
    }
    it->second.last_heartbeat = clock_.now();
    return true;
}

bool WorkerRegistry::try_allocate(WorkerId id, const ResourceRequest& req, JobId job) {
    std::lock_guard lock(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end() || !it->second.alive) {
        return false;
    }
    WorkerInfo& w = it->second;
    if (w.running_jobs.contains(job)) {
        return false;
    }
    if (w.available.cpu_units < req.cpu_units || w.available.memory_mb < req.memory_mb) {
        return false;
    }
    w.available.cpu_units -= req.cpu_units;
    w.available.memory_mb -= req.memory_mb;
    w.running_jobs.insert(job);
    return true;
}

bool WorkerRegistry::release(WorkerId id, const ResourceRequest& req, JobId job) {
    std::lock_guard lock(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end() || !it->second.alive) {
        return false;  // Death sweep already reclaimed everything.
    }
    WorkerInfo& w = it->second;
    if (w.running_jobs.erase(job) == 0) {
        return false;  // Not running here: refuse a double-release.
    }
    // Defensive clamp: available can never exceed total.
    w.available.cpu_units = std::min(w.available.cpu_units + req.cpu_units,
                                     w.total.cpu_units);
    w.available.memory_mb = std::min(w.available.memory_mb + req.memory_mb,
                                     w.total.memory_mb);
    return true;
}

std::optional<WorkerId> WorkerRegistry::find_fit(const ResourceRequest& req,
                                                 std::optional<WorkerId> exclude) const {
    std::lock_guard lock(mutex_);
    std::optional<WorkerId> best;
    std::uint32_t best_free_cpu = 0;
    for (const auto& [id, w] : workers_) {
        if (!w.alive || (exclude && id == *exclude)) {
            continue;
        }
        if (w.available.cpu_units < req.cpu_units ||
            w.available.memory_mb < req.memory_mb) {
            continue;
        }
        // Most free CPU wins (spread load); lowest id breaks ties.
        if (!best || w.available.cpu_units > best_free_cpu ||
            (w.available.cpu_units == best_free_cpu && id.value() < best->value())) {
            best = id;
            best_free_cpu = w.available.cpu_units;
        }
    }
    return best;
}

std::optional<WorkerId> WorkerRegistry::find_reservation_target(
    const ResourceRequest& req) const {
    std::lock_guard lock(mutex_);
    std::optional<WorkerId> best;
    std::uint32_t best_total_cpu = 0;
    for (const auto& [id, w] : workers_) {
        if (!w.alive) {
            continue;
        }
        if (w.total.cpu_units < req.cpu_units || w.total.memory_mb < req.memory_mb) {
            continue;  // Could never fit, even empty.
        }
        if (!best || w.total.cpu_units > best_total_cpu ||
            (w.total.cpu_units == best_total_cpu && id.value() < best->value())) {
            best = id;
            best_total_cpu = w.total.cpu_units;
        }
    }
    return best;
}

std::vector<DeadWorker> WorkerRegistry::collect_dead(Duration timeout) {
    std::vector<DeadWorker> dead;
    std::vector<Event> events;
    {
        std::lock_guard lock(mutex_);
        const TimePoint now = clock_.now();
        for (auto& [id, w] : workers_) {
            if (!w.alive || now - w.last_heartbeat <= timeout) {
                continue;
            }
            w.alive = false;
            w.available = ResourceCapacity{0, 0};  // Nothing schedulable here.

            DeadWorker entry;
            entry.id = id;
            entry.orphaned_jobs.assign(w.running_jobs.begin(), w.running_jobs.end());
            w.running_jobs.clear();
            dead.push_back(std::move(entry));

            Event event{};
            event.type = EventType::WorkerMarkedDead;
            event.timestamp = now;
            event.worker_id = id;
            event.detail = w.name;
            events.push_back(std::move(event));
        }
    }
    for (const Event& e : events) {
        bus_.publish(e);  // Outside the lock, as everywhere.
    }
    return dead;
}

std::optional<WorkerInfo> WorkerRegistry::get(WorkerId id) const {
    std::lock_guard lock(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<WorkerInfo> WorkerRegistry::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<WorkerInfo> out;
    out.reserve(workers_.size());
    for (const auto& [id, w] : workers_) {
        out.push_back(w);
    }
    return out;
}

std::size_t WorkerRegistry::alive_count() const {
    std::lock_guard lock(mutex_);
    std::size_t n = 0;
    for (const auto& [id, w] : workers_) {
        if (w.alive) {
            ++n;
        }
    }
    return n;
}

}  // namespace chronos
