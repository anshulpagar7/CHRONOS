# ADR-004: Score-indexed ordered set + periodic rescoring (not a priority heap)

**Status:** Accepted · **Date:** Day 2

## Context

The obvious container for "pick the best job" is `std::priority_queue`.
v1 used one — with a comparator that called `steady_clock::now()` and mixed
in deadline urgency and aging. That design has three fatal problems:

1. **Undefined behaviour.** A heap comparator must be a strict weak ordering
   that is *stable during a heap operation*. A comparator whose answer
   changes between invocations (because `now()` moved) can corrupt the heap.
2. **Decorative aging.** A heap never re-compares elements after insertion.
   A job buried at the bottom never "ages up", no matter what the
   comparator says. v1's fairness feature could not work even in principle.
3. **No removal, no scanning.** Heaps can't remove arbitrary elements
   (cancellation) or iterate best→worst (resource-fit scanning).

## Decision

`ReadySet` = `std::set<Entry, BestFirst>` of `(score, id)` entries —
descending score, ascending-id tie-break for deterministic, stable ordering —
plus an `unordered_map<JobId, iterator>` index for O(log n) removal.

- **Scores are computed at insertion and refreshed by `rescore()`**, which
  the scheduler calls on a coarse tick with fresh job snapshots. Between
  ticks, scores are intentionally stale: aging changes ordering on the order
  of seconds, so re-sorting on every operation would buy nothing. Rescoring
  is O(n log n) with small n (queued jobs only) at ~1 Hz — negligible.
- **The set stores ids + cached scores, never Job copies** (ADR-003: JobStore
  stays the single source of truth).
- **`best_where(eligible, max_scan)`** scans best→worst for the first
  eligible job (Phase 3: "some worker has capacity for it"), with a bound so
  a pathological queue can't turn one dispatch into an O(n) predicate storm.
- **Skip accounting.** When a pick succeeds, every higher-scored entry it
  jumped past has its skip count incremented (surviving rescore, cleared on
  removal). A round where *nothing* fits records no skips -- saturation is
  not starvation, and this distinction stops the backfill guard from firing
  spuriously on a merely-busy cluster. The
  Phase-3 backfill guard promotes a repeatedly-skipped top job into a
  capacity *reservation* — Slurm-style backfilling — so large jobs cannot
  be starved by an endless stream of small ones that fit.

## Alternatives considered

- **Re-heapify a `priority_queue` periodically:** fixes staleness but still
  no removal, no scanning, and rebuild is O(n) allocation churn.
- **Lazy deletion (tombstones) over a heap:** classic, but tombstone floods
  under heavy cancellation and still no best→worst scan.
- **Bucket-per-priority round-robin:** O(1) and great for pure priority, but
  cannot express continuous scores (deadline urgency, aging) at all.

## Consequences

- Aging is real and provable: a unit test flips the ordering of a starving
  job purely by advancing a `SimulatedClock` and calling `rescore()`.
- All operations O(log n); ordering fully deterministic (simulator-safe).
- `ReadySet` is deliberately not thread-safe: it is owned by the single
  scheduler thread (Phase 3), keeping the hot path lock-free.
