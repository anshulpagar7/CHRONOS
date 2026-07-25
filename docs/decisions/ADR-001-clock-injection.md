# ADR-001: Inject the clock everywhere

**Status:** Accepted · **Date:** Day 1

## Context

Nearly every interesting behaviour in a scheduler is time-dependent: retry
backoff, deadline urgency, priority aging, heartbeat timeouts, queue-latency
metrics. Code that calls `std::chrono::steady_clock::now()` directly is
untestable except with `sleep()`-based tests, which are slow, flaky, and can
never assert exact timing ("eligible after *exactly* 2s of backoff").

## Decision

No component in CHRONOS calls `now()` on a global clock. Every time-dependent
component receives a `Clock&` (an abstract interface) at construction.

Two implementations exist:

- `SystemClock` — wraps `std::chrono::steady_clock`. Used in production.
  Steady (monotonic) rather than wall clock, so scheduling decisions are
  immune to NTP corrections and daylight-saving jumps.
- `SimulatedClock` — time moves only when explicitly advanced. Used by the
  unit tests and by the scheduling simulator.

## Consequences

- **Deterministic tests.** `clock.advance(2s)` then assert — zero sleeps,
  zero flakiness, exact assertions on timestamps and backoff windows.
- **The simulator becomes possible.** The same production scheduler code can
  process tens of thousands of synthetic jobs in milliseconds of wall time
  by driving a `SimulatedClock`, enabling apples-to-apples policy
  comparisons (Phase 4).
- **Cost:** one extra constructor parameter per component, and one virtual
  call per `now()`. Negligible against everything it buys.

One deliberate exception: the structured logger stamps lines with
`system_clock` (wall time), because humans correlate logs against wall
clocks, and log timestamps drive no scheduling decision.
