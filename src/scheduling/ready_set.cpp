#include "chronos/scheduling/ready_set.h"

namespace chronos {

void ReadySet::add(const Job& job, TimePoint now) {
    auto existing = index_.find(job.id);
    if (existing != index_.end()) {
        entries_.erase(existing->second);
        index_.erase(existing);
    }
    const auto [it, inserted] = entries_.insert({policy_.score(job, now), job.id});
    index_.emplace(job.id, it);
}

bool ReadySet::remove(JobId id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return false;
    }
    entries_.erase(it->second);
    index_.erase(it);
    skips_.erase(id);
    return true;
}

bool ReadySet::contains(JobId id) const {
    return index_.contains(id);
}

std::optional<JobId> ReadySet::best() const {
    if (entries_.empty()) {
        return std::nullopt;
    }
    return entries_.begin()->id;
}

std::optional<JobId> ReadySet::best_where(const std::function<bool(JobId)>& eligible,
                                          std::size_t max_scan) {
    // Collect passed-over entries first; their skip counts increment ONLY
    // if something below them is actually picked. Being passed over in a
    // fully-saturated round (nothing fits anyone) is not starvation -- a
    // skip means "a lower-scored job jumped past you". This distinction
    // keeps the backfill guard from firing spuriously under saturation.
    std::vector<JobId> passed_over;
    std::size_t scanned = 0;
    for (const Entry& entry : entries_) {
        if (scanned++ >= max_scan) {
            break;
        }
        if (eligible(entry.id)) {
            for (const JobId id : passed_over) {
                ++skips_[id];
            }
            return entry.id;
        }
        passed_over.push_back(entry.id);
    }
    return std::nullopt;
}

int ReadySet::skip_count(JobId id) const {
    auto it = skips_.find(id);
    return it == skips_.end() ? 0 : it->second;
}

void ReadySet::rescore(const std::vector<Job>& snapshots, TimePoint now) {
    for (const Job& job : snapshots) {
        auto it = index_.find(job.id);
        if (it == index_.end()) {
            continue;  // Not queued here; ignore.
        }
        entries_.erase(it->second);
        const auto [entry_it, inserted] = entries_.insert({policy_.score(job, now), job.id});
        it->second = entry_it;
    }
}

std::vector<std::pair<double, JobId>> ReadySet::entries() const {
    std::vector<std::pair<double, JobId>> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) {
        out.emplace_back(e.score, e.id);
    }
    return out;
}

}  // namespace chronos
