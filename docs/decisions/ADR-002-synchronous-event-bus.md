# ADR-002: Synchronous EventBus as the observability backbone

**Status:** Accepted · **Date:** Day 1

## Context

CHRONOS needs structured logging, metrics, and a per-job execution timeline.
Implementing each as ad-hoc calls sprinkled through the scheduler couples the
core to its observers and triples the number of call sites to maintain.

## Decision

Every observable occurrence (job submitted, state changed, and later:
dispatched, worker registered, heartbeat missed, …) is published as a
structured `Event` on a single `EventBus`. Logging, metrics, and the timeline
recorder are all just subscribers. The core never knows they exist.

Delivery is **synchronous**, in subscription order, on the publisher's thread.

Concurrency contract:

- `subscribe` / `unsubscribe` / `publish` are safe from any thread
  (`shared_mutex`: publishes take shared locks and run concurrently;
  subscription changes take exclusive locks).
- `publish` snapshots the handler list, then invokes handlers *outside* the
  lock, so a handler may re-enter the bus without deadlocking.
- Corollary: a handler racing with its own `unsubscribe` may observe one
  final event. Documented and accepted.
- Publishers (e.g. `JobStore`) must release their own internal locks before
  publishing, so subscribers can safely call back into the publisher.

## Alternatives considered

- **Async bus (queue + dispatcher thread):** decouples publisher latency
  from subscriber cost, but destroys deterministic event ordering — which
  the simulator and tests rely on — and adds a thread + queue + shutdown
  protocol on day one.

## Consequences

- One mechanism feeds three features (logs, metrics, timeline).
- Event order exactly matches causal order — trivially debuggable,
  simulator-friendly, and testable with plain vectors.
- A slow subscriber blocks the publisher. Acceptable: all built-in
  subscribers are O(1) appends/increments. If an expensive sink ever
  appears (e.g. network shipping), it wraps itself in its own queue —
  the bus interface doesn't change.
