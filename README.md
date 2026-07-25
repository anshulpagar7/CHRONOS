# CHRONOS

**A production-inspired task scheduling & execution engine in modern C++20.**

Resource-aware, policy-pluggable scheduling over a pool of workers, with
retry/backoff fault tolerance, an enforced job state machine, and first-class
observability. Architected from day one so the in-process worker pool can be
swapped for remote workers over gRPC without touching the scheduler.

![CI](https://img.shields.io/badge/CI-4_preset_matrix-3FB68B) ![tests](https://img.shields.io/badge/tests-155_passing-3FB68B) ![sanitizers](https://img.shields.io/badge/ASan%20%7C%20UBSan%20%7C%20TSan-clean-3FB68B) ![std](https://img.shields.io/badge/C%2B%2B-20-5B8DEF) ![deps](https://img.shields.io/badge/dependencies-zero-E8A33D) ![license](https://img.shields.io/badge/license-MIT-8A93A6)

> Status: **complete (v1.0)** — the engine runs end to end (push-based
> scheduling, resource-aware dispatch, crash detection + job rescue,
> backfill reservations), proves itself (Prometheus metrics, a
> deterministic policy simulator, ~49k jobs/s measured), and is operable:
> REST API, live dashboard, one-command Docker demo. Stress-tested for
> conservation invariants under chaos; all sanitizer-clean; zero
> dependencies. See the
> [build phases](docs/architecture.md#build-phases).

## What exists today

- **`JobStore`** — single source of truth for every job. All state changes
  flow through an enforced state machine (illegal transitions throw), every
  change is timestamped into a per-job execution timeline, and every change
  emits a structured event.
- **`EventBus`** — synchronous, causally ordered pub/sub; logging (and later
  metrics + timeline) are just subscribers. The core never knows they exist.
- **`Clock` / `SimulatedClock`** — no component reads wall time directly, so
  every time-dependent behaviour is deterministic and exactly testable.
- **Structured logging** — zero-dependency, thread-safe, `key=value` lines
  (`grep job_id=42` reconstructs one job's entire story).
- **Pluggable scheduling policies** — FIFO, Priority, EDF, and a weighted
  Composite (priority + bounded deadline urgency + aging + retry penalty).
  Aging is *real*: the ready set re-scores on a tick, and a unit test proves
  a starving job overtakes fresher high-priority work.
- **`ReadySet`** — score-indexed ordered set: O(log n) removal
  (cancellation), best→worst eligibility scanning with a scan bound, and
  skip counting that feeds Phase-3 Slurm-style backfill reservations.
- **`RetryManager`** — capped exponential backoff with deterministic,
  seedable jitter; dead-letters when the attempt budget is spent; exposes
  `next_due()` so the scheduler can sleep until exactly the next retry.
- **`Scheduler`** — a single scheduling thread (actor-style command queue)
  push-dispatches the best eligible job to the best-fitting worker;
  condition-variable driven, with drain shutdown. The whole iteration is
  `run_once(now)`, so tests and the simulator drive the *production*
  scheduler deterministically.
- **`WorkerRegistry` + `ExecutionBackend`** — resource accounting
  (allocate/release, atomic check-and-reserve), and the two-interface seam
  that makes local worker threads swappable for remote gRPC workers.
- **Fault tolerance** — sidecar heartbeats, lease-based death detection,
  automatic rescue of orphaned jobs (chaos-tested: a worker is hard-killed
  mid-execution and its job completes elsewhere), retry with capped
  jittered backoff, dead-lettering.
- **Backfill reservations** — a repeatedly-skipped large job freezes the
  one worker that could host it; small jobs keep backfilling the rest of
  the fleet. Bounded starvation, tested.
- **Observability as subscribers** — `MetricsRegistry` (counters, gauges,
  bucketed histograms with percentile estimation, native Prometheus text
  exposition), `SchedulerMetrics` deriving the full metric set from the
  event stream, and a `TimelineRecorder` ring. The core never knows any of
  it exists.
- **A deterministic scheduling simulator** — the *production* scheduler
  driven on simulated time: 10k jobs and ~12 minutes of simulated wall
  clock in ~7 s, bit-identical per seed. `chronos-sim --compare` reproduces
  classical scheduling theory on real code (FIFO fairest, Priority starves,
  EDF minimizes deadline misses, Composite blends best-of) — see
  [docs/benchmarks.md](docs/benchmarks.md).
- **Measured performance** — ~49,000 jobs/s sustained through the full
  threaded stack on a single core; ReadySet ops in the millions/s
  (`chronos-bench`).
- **A REST API + live dashboard** — `chronosd` serves `POST /api/jobs`,
  per-job timelines, cluster state, a live event feed, and `/metrics`
  (Prometheus text format) from a zero-dependency HTTP/JSON layer
  (ADR-010), plus a single-file dashboard with the job state machine
  rendered live: queue pressure, worker utilization, backfill
  reservations, one-click submit/cancel.
- **155 tests** (deterministic unit + real-thread integration/chaos +
  raw-TCP API tests + conservation-under-chaos stress), run
  in CI under Debug, Release, **ASan+UBSan and TSan**, compiled with
  `-Wall -Wextra -Wconversion … -Werror`.

## Quick start

```bash
cmake --preset release
cmake --build --preset release -j"$(nproc)"
ctest --preset release          # run the test suite
./build/release/day1_demo       # watch a job lifecycle as structured logs
./build/release/day2_policy_demo  # four policies rank one workload
./build/release/day3_demo       # kill a worker mid-job, watch the rescue
./build/release/chronos-sim --compare --jobs=10000   # policy shootout
./build/release/chronos-bench   # throughput + latency numbers
```

Sanitizer runs: swap `release` for `asan` or `tsan`.

Demo output:

```
…Z INFO job submitted job_id=2 detail=train-model
…Z INFO job state changed job_id=2 from=QUEUED to=DISPATCHED attempt=0
…Z INFO job state changed job_id=2 from=DISPATCHED to=RUNNING attempt=1
…Z INFO job state changed job_id=2 from=RUNNING to=RETRY_WAIT attempt=1
…Z WARN state machine rejected transition what="illegal transition for job 1: COMPLETED -> RUNNING"
```

## Design

Start with [docs/architecture.md](docs/architecture.md), then the decision
records:

- [ADR-001 — Inject the clock everywhere](docs/decisions/ADR-001-clock-injection.md)
- [ADR-002 — Synchronous EventBus as the observability backbone](docs/decisions/ADR-002-synchronous-event-bus.md)
- [ADR-003 — JobStore + enforced state machine](docs/decisions/ADR-003-job-state-machine.md)
- [ADR-004 — ReadySet: ordered set + rescoring, not a heap](docs/decisions/ADR-004-ready-set.md)
- [ADR-005 — Retry semantics: capped backoff with jitter](docs/decisions/ADR-005-retry-semantics.md)
- [ADR-006 — Push dispatch & the ExecutionBackend seam](docs/decisions/ADR-006-push-dispatch-backend-seam.md)
- [ADR-007 — Liveness, rescue, and backfill reservations](docs/decisions/ADR-007-liveness-rescue-backfill.md)
- [ADR-008 — Observability as EventBus subscribers](docs/decisions/ADR-008-observability.md)
- [ADR-009 — A simulator around the production scheduler](docs/decisions/ADR-009-simulator.md)
- [ADR-010 — A hand-rolled HTTP/JSON layer instead of a framework](docs/decisions/ADR-010-zero-dependency-http.md)

## Layout

```
include/chronos/   public headers (core/, observability/, …)
src/               implementations
apps/              demos and, later, the chronosd daemon + simulator
tests/unit/        GoogleTest suite (deterministic via SimulatedClock)
docs/              architecture + ADRs
```

## License

MIT
