# ADR-005: Retry semantics — capped exponential backoff with jitter

**Status:** Accepted · **Date:** Day 2

## Context

Failed jobs must be retried without (a) hammering whatever just failed,
(b) synchronized retry storms, or (c) retrying forever. Retry timing is also
exactly the kind of logic that is untestable unless it is deterministic.

## Decision

`RetryManager` owns retry policy end to end:

- **Attempt budget.** `spec.max_retries` = *additional* runs after the
  first, so a job executes at most `max_retries + 1` times. With
  `job.attempt` = runs started (maintained by JobStore on each entry into
  `Running`): a failure with `attempt <= max_retries` retries; with
  `attempt == max_retries + 1` the job dead-letters (`Failed`).
- **Backoff.** `delay = min(base · multiplier^(attempt−1), max_backoff)`,
  computed in double space so a large attempt number saturates at the cap
  instead of overflowing (unit-tested at attempt 30).
- **Jitter.** The delay is scaled by a factor drawn uniformly from
  `[1−j, 1+j]`. Without jitter, all jobs orphaned by one worker death retry
  at the same instant — a thundering herd against the scheduler and against
  whatever downstream dependency broke. Multiplicative (proportional) jitter
  keeps the spread meaningful at every backoff scale. The ceiling still
  wins: jittered delays clamp to `max_backoff`.
- **Deterministic randomness.** Jitter comes from a seeded `mt19937_64`
  (fixed default seed). Identical config ⇒ identical delays ⇒ reproducible
  simulator runs and exact tests; production can inject entropy via the
  seed if desired.
- **The pending queue lives here.** A min-heap of `(eligible_at, id)` with
  `schedule()` / `collect_due(now)` / `next_due()`. `next_due()` exists so
  the Phase-3 scheduler thread can sleep on its condition variable until
  *exactly* the next retry ripens — no polling loop.
- `RetryWait` is a first-class job state (ADR-003), not a hidden timer, so
  backoff is visible to the timeline, metrics, and dashboard.

## Consequences

- Every property is exactly unit-tested on simulated time: the retry/give-up
  boundary, per-attempt doubling, cap saturation, jitter bounds, and
  seed-determinism (two managers, same config, identical decisions).
- Single-threaded by design (scheduler-owned), like ReadySet.
- Trade-off: one shared backoff config for all jobs. Per-job or per-class
  retry configs can be added later by keying configs in `decide()`; the
  interface doesn't change.
