# ADR-006: Push-based dispatch through an ExecutionBackend seam

**Status:** Accepted · **Date:** Day 3

## Context

Two coupled decisions: (1) do workers pull work, or does the scheduler push
it? (2) where exactly is the boundary that later lets local worker threads
be replaced by remote workers over gRPC without redesigning the scheduler?

v1 was pull-based: worker threads polled `get_next_job()` under a global
lock. Pulling has a structural flaw for resource-aware scheduling: the
decision "which job?" is made per-asking-worker, so the scheduler can never
match the *globally best* job against *all* workers' capacities — and every
idle worker hammers the scheduler lock on a 100ms poll loop.

## Decision

**Push, from a single scheduler thread.** One thread owns all scheduling
state (ReadySet, RetryManager, the backfill reservation) and actively pairs
(best eligible job) × (best-fitting worker), then pushes a `JobAssignment`
into that worker's inbox. Everyone else — `submit()`, `cancel()`, transport
callbacks — talks to the scheduler through a command queue (an actor, in
effect). This is the same shape as the Kubernetes scheduler.

**The seam is two small interfaces:**

- `ExecutionBackend` (downward): one operation, `dispatch(JobAssignment)` —
  "deliver this to that worker's inbox". The assignment carries a full job
  snapshot, so the transport never touches the JobStore — exactly what a
  serialized message will carry over a wire.
- `SchedulerClient` (upward): `report_started` / `report_completion` /
  `report_heartbeat` — what a remote worker would send back over its stream.

Workers themselves are *records* in the `WorkerRegistry` (id, capacity,
liveness, running jobs) — never threads or sockets. `LocalThreadBackend`
implements the inbox as an in-process queue + thread; a future
`GrpcBackend` implements it as a per-worker stream. Nothing above the seam
changes — that is the falsifiable claim this architecture makes.

## Consequences

- Resource-aware matching is possible at all (the pull model cannot do it).
- Near-zero contention: scheduling state needs no locks (single owner);
  cross-thread traffic is a short command-queue critical section.
- Determinism: `run_once(now)` contains the entire iteration and is public,
  so unit tests and the Phase-4 simulator drive the *production* scheduler
  on a SimulatedClock with no threads and no sleeps.
- Trade-off: the scheduler thread serializes decisions. At target scale
  (thousands of jobs/s, µs-scale iterations) this is far from the
  bottleneck, and it buys correctness that a lock-sharded design would
  fight for.
- Stale-report handling falls out of the state machine: a dead worker's
  late `report_completion` finds its job no longer `Running` (it was
  rescued) and is logged and ignored — no special-case bookkeeping.
