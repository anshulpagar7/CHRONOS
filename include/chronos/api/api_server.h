#pragma once

#include <cstdint>
#include <string>

#include "chronos/api/http_server.h"
#include "chronos/core/clock.h"
#include "chronos/core/job_store.h"
#include "chronos/execution/worker_registry.h"
#include "chronos/observability/metrics.h"
#include "chronos/observability/timeline_recorder.h"
#include "chronos/scheduling/scheduler.h"

namespace chronos::api {

/// The REST surface over a running CHRONOS system.
///
///   GET  /api/state            cluster overview: job counts, workers,
///                              queue depth, active backfill reservation
///   GET  /api/jobs?limit=N&state=S   newest-first job list
///   GET  /api/jobs/{id}        one job, full transition timeline
///   POST /api/jobs             submit  {name, priority, cpu, memory_mb,
///                              max_retries, deadline_ms?, payload?}
///   POST /api/jobs/{id}/cancel
///   GET  /api/events?limit=N   recent system events (timeline ring)
///   GET  /metrics              Prometheus text exposition
///   GET  /...                  static dashboard files (if dir configured)
///
/// Read endpoints serve store/registry snapshots; writes go through the
/// Scheduler's thread-safe command interface. The HTTP worker pool
/// therefore needs no locking of its own.
class ApiServer {
public:
    ApiServer(Scheduler& scheduler, JobStore& store, WorkerRegistry& registry,
              MetricsRegistry& metrics, TimelineRecorder& timeline, Clock& clock,
              std::uint16_t port, std::string static_dir = "");

    void start() { http_.start(); }
    void stop() { http_.stop(); }
    [[nodiscard]] std::uint16_t port() const { return http_.port(); }

private:
    Router build_router();

    HttpResponse get_state(const HttpRequest&) const;
    HttpResponse list_jobs(const HttpRequest&) const;
    HttpResponse get_job(const HttpRequest&) const;
    HttpResponse submit_job(const HttpRequest&);
    HttpResponse cancel_job(const HttpRequest&);
    HttpResponse get_events(const HttpRequest&) const;
    HttpResponse get_metrics(const HttpRequest&) const;
    HttpResponse serve_static(const HttpRequest&) const;

    Scheduler& scheduler_;
    JobStore& store_;
    WorkerRegistry& registry_;
    MetricsRegistry& metrics_;
    TimelineRecorder& timeline_;
    Clock& clock_;
    std::string static_dir_;

    HttpServer http_;
};

}  // namespace chronos::api
