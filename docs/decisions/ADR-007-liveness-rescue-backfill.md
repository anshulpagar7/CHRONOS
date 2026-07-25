# ADR-007: Lease-based liveness, job rescue, and backfill reservations

**Status:** Accepted · **Date:** Day 3

## Liveness: heartbeat leases, sidecar beats, permanent death

Workers hold a *lease*: the transport heartbeats on their behalf; a worker
silent longer than `heartbeat_timeout` is marked dead by the scheduler's
sweep, its resource account is zeroed, and its in-flight jobs are returned
for rescue.

Two subtleties worth recording:

- **The heartbeat is a sidecar, not part of the work loop.** A worker
  executing a 10-minute job cannot beat from inside its run loop — it would
  read as dead mid-job. `LocalThreadBackend` therefore runs one heartbeat
  thread beating for every living worker (killed workers are excluded,
  which is precisely how a simulated crash goes silent). A remote worker
  would do the same with a background thread beating over its stream.
- **Death is permanent.** A worker that resumes beating after being marked
  dead is *not* resurrected: its jobs may already be rescheduled elsewhere,
  so accepting it back creates split-brain (two workers executing the same
  job, both reporting). A revived transport must re-register as a new
  worker. Its late reports for rescued jobs bounce off the state machine
  (job no longer `Running`) and are ignored.

## Rescue

Orphaned jobs requeue according to how far they got, and the state machine
(ADR-003) already encodes the difference:

- `Dispatched → Queued`: never started; the attempt is **not** consumed.
- `Running → Queued`: died mid-run; the attempt **is** consumed. Rescue is
  deliberately *not* a retry — no backoff, no retry-budget charge beyond
  the consumed attempt — because the job did nothing wrong; the
  infrastructure did.

## Backfill reservations (anti-starvation for large jobs)

Resource-aware dispatch has a classic failure mode: the top-scored job
needs 8 CPUs, no worker currently has 8 free, so smaller jobs keep slipping
past it — and since each small dispatch re-occupies capacity, the big job
can starve *forever* despite having the highest score. Aging cannot fix
this: score is not the problem; fit is.

Mechanism (a deliberately simplified version of Slurm's backfill
scheduling):

1. `ReadySet::best_where()` counts how often each job is *jumped past by a
   lower-scored job that dispatches* while it cannot fit (skip counts
   survive rescoring, reset on removal). Rounds where nothing fits anyone
   record no skips: saturation is not starvation.
2. When the queue head's skip count reaches `backfill_skip_threshold`, the
   scheduler places a **reservation**: it picks the worker whose *total*
   capacity could ever host the job (largest such, by design the fastest to
   drain proportionally) and freezes it — that worker accepts only the
   reserved job.
3. Other jobs continue to dispatch to other workers (that is the "backfill"
   part: the cluster does not idle while the big job waits).
4. The reserved worker drains; the reserved job dispatches; the reservation
   clears. It also clears if the reserved job is cancelled or the reserved
   worker dies.

Full Slurm backfill computes *time-based* reservations ("job X starts at
T; run anything that finishes before T"), which requires execution-time
estimates we do not have. The capacity-freeze variant needs no estimates
and provides the property that matters: **bounded starvation** — once
reserved, the big job's wait is bounded by the runtime of jobs already on
its worker. One reservation at a time keeps the policy analyzable; a
reservation *queue* is a possible extension.

All three mechanisms are unit-tested deterministically (SimulatedClock +
run_once) and exercised under real threads by the integration chaos test
(`kill_worker` mid-execution → detected → rescued → completed), which runs
clean under TSan.
