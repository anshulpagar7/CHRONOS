# Benchmarks & Simulation Results

All numbers reproduce deterministically (fixed seeds) with the commands
shown. Reference environment: single-core Ubuntu 24 container, GCC 13.3,
Release build. More cores only improve the threaded results.

## Policy comparison (simulator)

10,000 synthetic jobs through the **production scheduler** on simulated
time — ~715 s of simulated wall clock in ~7 s of real time per policy.
Workload: Poisson arrivals at 14 jobs/s (~88% cluster utilization —
feasible but contended), priorities 0–9, 50% deadlines in [2 s, 30 s],
1–4 cpu / 64–512 MB per job, 8% per-attempt failure probability,
3 retries. Cluster: 16+8+8 cpu across three workers.

```
./build/release/chronos-sim --compare --jobs=10000
```

| policy    | wait p50 | wait p99 | deadline miss | Jain fairness |
|-----------|---------:|---------:|--------------:|--------------:|
| fifo      |   170 ms |  2904 ms |          0.7% |     **0.683** |
| priority  |    58 ms | 10200 ms |          1.2% |         0.316 |
| edf       |    71 ms |  5613 ms |      **0.2%** |         0.539 |
| composite |  **51 ms** |  8918 ms |      **0.2%** |         0.294 |

The table reproduces classical scheduling theory:

- **FIFO** is the fairest and has the tightest tail — and the worst
  median, since nothing urgent ever jumps the line.
- **Priority** gives its favourites a great median while its p99 explodes
  and fairness craters: low-priority starvation, exactly as predicted.
- **EDF** minimizes deadline misses — the one property it exists for.
- **Composite** (priority + bounded urgency + aging − retry penalty) takes
  the best median *and* ties EDF's deadline rate; its aging term pulls the
  starvation tail below pure Priority's (p99 8.9 s vs 10.2 s).

One workload note that is itself a finding: under heavy *overload* (try
`--arrival-hz=50`) the policies converge — whatever physically cannot be
done is missed by everyone, and EDF's edge washes out (the classic
EDF-under-overload domino effect). Policy choice matters most in the
feasible-but-contended regime.

## End-to-end throughput (real threads)

Full stack — scheduler thread, `LocalThreadBackend` worker threads,
sidecar heartbeats, metrics subscriber — with a no-op executor:

```
./build/release/chronos-bench 5000 4
```

```
end-to-end : 5000 no-op jobs, 4 workers -> 0.10 s (~49,000 jobs/s sustained)
```

(The reported per-visit queue waits under this run measure burst queueing —
all 5,000 jobs are submitted at once — not per-decision latency.)

## ReadySet microbenchmark

100,000 entries, `PriorityPolicy` scoring:

```
add 2.9M ops/s | best 73.1M ops/s | remove 7.4M ops/s
```

O(log n) ordered-set operations comfortably clear the dispatch loop's
needs by 3–4 orders of magnitude; the ready structure is nowhere near the
bottleneck.
