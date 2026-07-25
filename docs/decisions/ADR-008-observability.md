# ADR-008: Observability as EventBus subscribers; bucketed histograms

**Status:** Accepted · **Date:** Day 4

## Context

CHRONOS needs operational metrics (counters, gauges, latency distributions),
a per-job execution timeline, and a Prometheus-compatible endpoint — without
the core scheduling code knowing any of it exists.

## Decision

**Everything observes; nothing is observed *by* the core.** `SchedulerMetrics`
and `TimelineRecorder` are plain `EventBus` subscribers (ADR-002 pays off):
the metric set is derived entirely from the event stream, latencies are
computed from *event timestamps* (so they are equally correct under the real
clock and the simulator's clock), and removing observability is deleting two
objects.

**Histograms are bucketed, Prometheus-style** — fixed upper bounds, a count
per bucket, plus sum and count — rather than raw-sample reservoirs:

- O(1) memory forever and O(log b) recording, regardless of job volume;
- renders natively to the Prometheus text exposition format (the Phase-5
  `/metrics` endpoint is just `render_prometheus()`);
- percentile estimates interpolate linearly within the containing bucket —
  plenty for dashboards and alerts. Where tests need exactness they assert
  on `count()`/`sum()`, which are precise.

Metric handles are get-or-create by name and live as long as the registry,
so hot paths hold direct `Counter&`/`Histogram&` references — no map lookups
per event.

## Consequences

- The simulator gets metrics for free and they are *deterministic*, because
  every timestamp is simulated time.
- Gauges for Queued/Running/RetryWait are maintained by state-transition
  deltas — O(1) per event, no store polling.
- Trade-off: no label support (one time series per name). Acceptable for a
  single-scheduler system; labels can be added behind the same interface
  when per-worker series are wanted.
