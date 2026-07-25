# ADR-009: A discrete-event simulator around the production scheduler

**Status:** Accepted · **Date:** Day 4

## Context

Claims like "the Composite policy prevents starvation" or "EDF minimizes
deadline misses" need *evidence* on nontrivial workloads. Wall-clock
experiments are slow (minutes per run), noisy, and unreproducible.

## Decision

`chronos::sim::Simulation` is a discrete-event harness around the **real**
scheduler — the exact production `Scheduler`, `ReadySet`, `RetryManager`,
`WorkerRegistry` code paths, driven by `run_once(now)` on a
`SimulatedClock`. Nothing scheduling-related is mocked; only *execution*
is simulated (`SimBackend::dispatch` schedules a completion event at
`now + duration` instead of running anything). This is precisely what
ADR-001 (clock injection) and ADR-006 (`run_once` as the whole iteration)
were built to enable.

Key mechanics:

- **Workload pre-generation.** Every random decision — arrival times
  (Poisson), priorities, deadlines, resource needs, execution durations,
  which attempts fail — is drawn up front from one seeded RNG into a
  `PlannedJob` table. Every policy therefore faces the *bit-identical*
  workload, and `(config, seed)` fully determines a run. Determinism is
  unit-tested (two runs, identical results to the double).
- **Event-leaping loop.** The clock jumps straight to the next event
  (arrival, completion, or retry ripening), so ~12 minutes of simulated
  time for 10k jobs costs ~7 s of wall time.
- **Statistics from the authoritative record.** Waits, turnarounds, and
  deadline outcomes are computed from `JobStore` histories — the same
  timeline the timeline recorder and API expose — not from side channels.
- **Fairness = Jain's index over completed-job turnarounds.** Waits are
  the wrong base: under healthy load most waits are exactly 0 and the
  index degenerates; turnaround is always positive and carries the same
  starvation signal.

## Consequences

- Policy claims in `docs/benchmarks.md` are reproducible to the bit.
- Regression tests can encode *behavioural* properties ("EDF misses no
  more deadlines than FIFO on this seed") — with the documented caveat
  that such properties only hold on *feasible* workloads; under overload
  all policies converge on missing the impossible, which the default
  workload config deliberately avoids (~88% utilization).
- The harness is the platform for the research-paper experiments
  (policy sweeps, utilization curves, seed ensembles).
- Limitation, recorded: worker failures are not yet simulated
  (`heartbeat_timeout` is effectively disabled in sim). Chaos-in-sim is a
  natural extension once failure traces matter for the paper.
