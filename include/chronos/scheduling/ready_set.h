#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include "chronos/core/ids.h"
#include "chronos/core/job.h"
#include "chronos/scheduling/scheduling_policy.h"

namespace chronos {

/// The set of Queued jobs, ordered best-first by policy score.
///
/// Why not std::priority_queue (v1's choice)? A binary heap cannot:
///   1. remove an arbitrary element        -> cancellation support,
///   2. re-evaluate scores after insertion -> aging that actually works
///      (v1's "aging" comparator was UB *and* decorative: nothing in a
///      heap is ever re-compared once placed),
///   3. iterate best->worst                -> resource-fit scanning.
///
/// Structure: an ordered std::set of (score, id) entries, descending by
/// score with ascending-id tie-break (stable, deterministic ordering), plus
/// an id->iterator index for O(log n) removal. Scores are computed at
/// insertion and refreshed via rescore() -- the scheduler calls it on a
/// periodic tick, which is what makes aging real. See ADR-004.
///
/// The set stores JobIds + cached scores only, never Job copies: JobStore
/// remains the single source of truth (ADR-003).
///
/// skip counting: best_where() records how many times each job was jumped
/// past by a *lower-scored* job that dispatched while it could not fit.
/// That -- and only that -- is starvation; being passed over in a round
/// where nothing fits anyone is just a busy cluster. The backfill guard
/// (ADR-007) turns these counts into Slurm-style capacity reservations.
/// Skip counts survive rescore() and reset only on removal.
///
/// Not internally synchronized: owned and accessed by the single scheduler
/// thread (Phase 3). Concurrency lives at the scheduler boundary, not here.
class ReadySet {
public:
    explicit ReadySet(const SchedulingPolicy& policy) : policy_(policy) {}

    /// Insert `job` scored at `now`. Re-adding a present id refreshes its
    /// score in place.
    void add(const Job& job, TimePoint now);

    /// Remove a job (dispatched or cancelled). Unknown ids are ignored.
    /// Clears the job's skip count. Returns true if it was present.
    bool remove(JobId id);

    [[nodiscard]] bool contains(JobId id) const;
    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
    [[nodiscard]] bool empty() const noexcept { return index_.empty(); }

    /// Highest-scoring job, if any.
    [[nodiscard]] std::optional<JobId> best() const;

    /// Highest-scoring job satisfying `eligible`, scanning best->worst over
    /// at most `max_scan` entries. If a pick succeeds, every higher-scored
    /// entry it jumped past has its skip count incremented; if nothing is
    /// eligible, no skips are recorded (a fully-saturated round is not
    /// starvation). Returns nullopt if nothing eligible within the bound.
    std::optional<JobId> best_where(const std::function<bool(JobId)>& eligible,
                                    std::size_t max_scan = SIZE_MAX);

    /// Times a lower-scored job was dispatched past this one. 0 if unknown.
    [[nodiscard]] int skip_count(JobId id) const;

    /// Recompute every score at `now` from the given snapshots (the
    /// scheduler passes fresh copies of all Queued jobs). Snapshots whose id
    /// is not currently in the set are ignored; members without a snapshot
    /// keep their stale score. O(n log n); called on a coarse tick.
    void rescore(const std::vector<Job>& snapshots, TimePoint now);

    /// (score, id) pairs, best-first. For tests, the API layer, and debug.
    [[nodiscard]] std::vector<std::pair<double, JobId>> entries() const;

private:
    struct Entry {
        double score;
        JobId id;
    };
    struct BestFirst {
        bool operator()(const Entry& a, const Entry& b) const noexcept {
            if (a.score != b.score) {
                return a.score > b.score;  // Higher score first.
            }
            return a.id.value() < b.id.value();  // Older job wins ties.
        }
    };

    const SchedulingPolicy& policy_;
    std::set<Entry, BestFirst> entries_;
    std::unordered_map<JobId, std::set<Entry, BestFirst>::iterator> index_;
    std::unordered_map<JobId, int> skips_;
};

}  // namespace chronos
