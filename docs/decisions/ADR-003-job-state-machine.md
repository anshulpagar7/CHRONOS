# ADR-003: JobStore as single source of truth with an enforced state machine

**Status:** Accepted · **Date:** Day 1

## Context

The v1 prototype kept up to three simultaneous copies of every job: one in a
lookup map, one inside a priority queue, and one local to the executing
worker — with no rule about which was authoritative. State updates applied to
one copy silently diverged from the others. This is the single largest bug
class in naive scheduler implementations.

Separately, v1 assigned job states by direct field writes (`job.state = …`),
so nothing prevented nonsense like `COMPLETED → RUNNING`.

## Decision

1. **`JobStore` owns the only canonical `Job` objects.** Every other
   component — the scheduler's ready set, workers, the API layer — holds
   `JobId`s or explicit snapshot copies. Accessors return copies, never
   references.

2. **Every state change goes through `JobStore::transition()`**, which:
   - validates the move against a static transition table
     (`transition_allowed`, a pure function, exhaustively unit-tested as a
     full 8×8 matrix);
   - throws `IllegalTransitionError` on violation — a scheduler bug surfaces
     as a loud exception at the faulty call site, never as silent state
     corruption;
   - stamps the change with the injected clock and appends it to the job's
     `history` — every job carries its own execution timeline for free;
   - increments `attempt` on each entry into `Running`;
   - emits a `JobStateChanged` event *after releasing the store's lock*.

## The state machine

```
Submitted ──> Queued ──> Dispatched ──> Running ──> Completed
                ^            │            │  │
                │            │            │  └────> Failed
                ├────────────┘ (rejected/ │
                │               pre-start └──> RetryWait ──> Queued
                │               worker death)
                └──────────── Running (mid-run worker death rescue)

Cancelled: reachable from every non-terminal state.
Terminal (no exits): Completed, Failed, Cancelled.
Self-transitions: always illegal (they are no-ops, hence bugs).
```

Two deliberate subtleties:

- `Dispatched → Queued` does **not** consume an attempt (the job never ran);
  `Running → Queued` (heartbeat rescue) keeps the consumed attempt. This
  distinction matters for retry accounting under worker failure.
- `RetryWait` models backoff as a *state*, not as a hidden timer, so the
  timeline, metrics, and dashboard all see it.

## Consequences

- The divergent-copies bug class is structurally impossible.
- Trade-off: one mutex around the store serializes state changes. Fine at
  target scale (µs-scale critical sections); the Phase-3 single-scheduler-
  thread design means the hot path has little contention anyway.
- Snapshot-copy accessors cost allocations; acceptable for correctness,
  and `history` copies are small.
