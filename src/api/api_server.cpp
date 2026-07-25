#include "chronos/api/api_server.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace chronos::api {

namespace {

double ms_since_epoch(TimePoint t) {
    return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count();
}

JsonValue job_to_json(const Job& job, bool include_history) {
    JsonValue out = JsonValue::object();
    out.set("id", job.id.value())
        .set("name", job.spec.name)
        .set("state", to_string(job.state))
        .set("priority", job.spec.priority)
        .set("attempt", job.attempt)
        .set("max_retries", job.spec.max_retries)
        .set("cpu", static_cast<std::uint64_t>(job.spec.resources.cpu_units))
        .set("memory_mb", static_cast<std::uint64_t>(job.spec.resources.memory_mb))
        .set("submitted_at_ms", ms_since_epoch(job.submit_time));
    if (job.spec.deadline) {
        out.set("deadline_at_ms", ms_since_epoch(*job.spec.deadline));
    }
    if (include_history) {
        JsonValue timeline = JsonValue::array();
        for (const StateChange& hop : job.history) {
            timeline.push(JsonValue::object()
                              .set("from", to_string(hop.from))
                              .set("to", to_string(hop.to))
                              .set("at_ms", ms_since_epoch(hop.at)));
        }
        out.set("timeline", std::move(timeline));
    }
    return out;
}

JsonValue event_to_json(const Event& e) {
    JsonValue out = JsonValue::object();
    out.set("type", to_string(e.type)).set("at_ms", ms_since_epoch(e.timestamp));
    if (e.job_id.value() != 0) {
        out.set("job_id", e.job_id.value());
    }
    if (e.worker_id.value() != 0) {
        out.set("worker_id", e.worker_id.value());
    }
    if (e.type == EventType::JobStateChanged) {
        out.set("from", to_string(e.from_state))
            .set("to", to_string(e.to_state))
            .set("attempt", e.attempt);
    }
    if (!e.detail.empty()) {
        out.set("detail", e.detail);
    }
    return out;
}

int parse_limit(const HttpRequest& request, int fallback, int max) {
    if (const auto raw = request.query_param("limit")) {
        const int parsed = std::atoi(raw->c_str());
        if (parsed > 0) {
            return std::min(parsed, max);
        }
    }
    return fallback;
}

}  // namespace

ApiServer::ApiServer(Scheduler& scheduler, JobStore& store, WorkerRegistry& registry,
                     MetricsRegistry& metrics, TimelineRecorder& timeline,
                     Clock& clock, std::uint16_t port, std::string static_dir)
    : scheduler_(scheduler),
      store_(store),
      registry_(registry),
      metrics_(metrics),
      timeline_(timeline),
      clock_(clock),
      static_dir_(std::move(static_dir)),
      http_(build_router(), port) {}

Router ApiServer::build_router() {
    Router router;
    router.add("GET", "/api/state",
               [this](const HttpRequest& r) { return get_state(r); });
    router.add("GET", "/api/jobs",
               [this](const HttpRequest& r) { return list_jobs(r); });
    router.add("GET", "/api/jobs/{id}",
               [this](const HttpRequest& r) { return get_job(r); });
    router.add("POST", "/api/jobs",
               [this](const HttpRequest& r) { return submit_job(r); });
    router.add("POST", "/api/jobs/{id}/cancel",
               [this](const HttpRequest& r) { return cancel_job(r); });
    router.add("GET", "/api/events",
               [this](const HttpRequest& r) { return get_events(r); });
    router.add("GET", "/metrics",
               [this](const HttpRequest& r) { return get_metrics(r); });
    if (!static_dir_.empty()) {
        router.add("GET", "/", [this](const HttpRequest& r) { return serve_static(r); });
        router.add("GET", "/{file}",
                   [this](const HttpRequest& r) { return serve_static(r); });
    }
    return router;
}

HttpResponse ApiServer::get_state(const HttpRequest&) const {
    JsonValue counts = JsonValue::object();
    for (const JobState state :
         {JobState::Submitted, JobState::Queued, JobState::Dispatched,
          JobState::Running, JobState::RetryWait, JobState::Completed,
          JobState::Failed, JobState::Cancelled}) {
        counts.set(to_string(state),
                   static_cast<std::uint64_t>(store_.snapshot_in_state(state).size()));
    }

    JsonValue workers = JsonValue::array();
    for (const WorkerInfo& w : registry_.snapshot()) {
        JsonValue running = JsonValue::array();
        for (const JobId job : w.running_jobs) {
            running.push(job.value());
        }
        workers.push(
            JsonValue::object()
                .set("id", w.id.value())
                .set("name", w.name)
                .set("alive", w.alive)
                .set("cpu_total", static_cast<std::uint64_t>(w.total.cpu_units))
                .set("cpu_free", static_cast<std::uint64_t>(w.available.cpu_units))
                .set("mem_total_mb", static_cast<std::uint64_t>(w.total.memory_mb))
                .set("mem_free_mb", static_cast<std::uint64_t>(w.available.memory_mb))
                .set("last_heartbeat_ms", ms_since_epoch(w.last_heartbeat))
                .set("running_jobs", std::move(running)));
    }

    JsonValue out = JsonValue::object();
    out.set("now_ms", ms_since_epoch(clock_.now()))
        .set("live_jobs", static_cast<std::uint64_t>(scheduler_.live_jobs()))
        .set("job_counts", std::move(counts))
        .set("workers", std::move(workers));
    if (const auto reservation = scheduler_.active_reservation()) {
        out.set("reservation", JsonValue::object()
                                   .set("job_id", reservation->first.value())
                                   .set("worker_id", reservation->second.value()));
    } else {
        out.set("reservation", nullptr);
    }
    return HttpResponse::json(out);
}

HttpResponse ApiServer::list_jobs(const HttpRequest& request) const {
    const int limit = parse_limit(request, 50, 500);
    const auto state_filter = request.query_param("state");

    std::vector<Job> jobs = store_.snapshot();
    if (state_filter) {
        jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                                  [&](const Job& job) {
                                      return *state_filter != to_string(job.state);
                                  }),
                   jobs.end());
    }
    // Newest first: ids are monotonic submission order.
    std::sort(jobs.begin(), jobs.end(),
              [](const Job& a, const Job& b) { return a.id.value() > b.id.value(); });
    if (jobs.size() > static_cast<std::size_t>(limit)) {
        jobs.resize(static_cast<std::size_t>(limit));
    }

    JsonValue list = JsonValue::array();
    for (const Job& job : jobs) {
        list.push(job_to_json(job, /*include_history=*/false));
    }
    return HttpResponse::json(
        JsonValue::object().set("jobs", std::move(list)));
}

HttpResponse ApiServer::get_job(const HttpRequest& request) const {
    const std::uint64_t raw_id =
        std::strtoull(request.path_params.at("id").c_str(), nullptr, 10);
    const auto job = store_.get(JobId{raw_id});
    if (!job) {
        return HttpResponse::error(404, "no such job");
    }
    return HttpResponse::json(job_to_json(*job, /*include_history=*/true));
}

HttpResponse ApiServer::submit_job(const HttpRequest& request) {
    JsonValue body;
    try {
        body = JsonValue::parse(request.body.empty() ? "{}" : request.body);
    } catch (const std::exception& e) {
        return HttpResponse::error(400, e.what());
    }
    if (!body.is_object()) {
        return HttpResponse::error(400, "expected a JSON object");
    }

    try {
        JobSpecBuilder builder;
        if (body.at("name").is_string()) {
            builder.name(body.at("name").as_string());
        }
        if (body.at("priority").is_number()) {
            builder.priority(static_cast<int>(body.at("priority").as_number()));
        }
        if (body.at("max_retries").is_number()) {
            builder.max_retries(static_cast<int>(body.at("max_retries").as_number()));
        }
        ResourceRequest resources{.cpu_units = 1, .memory_mb = 64};
        if (body.at("cpu").is_number()) {
            resources.cpu_units =
                static_cast<std::uint32_t>(body.at("cpu").as_number());
        }
        if (body.at("memory_mb").is_number()) {
            resources.memory_mb =
                static_cast<std::uint32_t>(body.at("memory_mb").as_number());
        }
        builder.resources(resources);
        if (body.at("deadline_ms").is_number()) {
            builder.deadline(clock_.now() +
                             std::chrono::milliseconds(static_cast<std::int64_t>(
                                 body.at("deadline_ms").as_number())));
        }
        if (body.at("payload").is_string()) {
            builder.payload(body.at("payload").as_string());
        }

        const JobId id = scheduler_.submit(builder.build());
        return HttpResponse::json(
            JsonValue::object()
                .set("id", id.value())
                .set("state", to_string(JobState::Queued)),
            201);
    } catch (const std::exception& e) {
        return HttpResponse::error(400, e.what());  // Builder validation.
    }
}

HttpResponse ApiServer::cancel_job(const HttpRequest& request) {
    const std::uint64_t raw_id =
        std::strtoull(request.path_params.at("id").c_str(), nullptr, 10);
    if (!store_.get(JobId{raw_id})) {
        return HttpResponse::error(404, "no such job");
    }
    const bool cancelled = scheduler_.cancel(JobId{raw_id});
    return HttpResponse::json(JsonValue::object().set("cancelled", cancelled));
}

HttpResponse ApiServer::get_events(const HttpRequest& request) const {
    const int limit = parse_limit(request, 100, 1000);
    JsonValue list = JsonValue::array();
    for (const Event& e : timeline_.recent(static_cast<std::size_t>(limit))) {
        list.push(event_to_json(e));
    }
    return HttpResponse::json(JsonValue::object().set("events", std::move(list)));
}

HttpResponse ApiServer::get_metrics(const HttpRequest&) const {
    return HttpResponse::text(metrics_.render_prometheus(), 200,
                              "text/plain; version=0.0.4; charset=utf-8");
}

HttpResponse ApiServer::serve_static(const HttpRequest& request) const {
    std::string file = request.path == "/" ? "index.html" : request.path.substr(1);
    // One flat directory of assets: reject anything that isn't a plain name.
    if (file.find('/') != std::string::npos ||
        file.find("..") != std::string::npos) {
        return HttpResponse::error(404, "not found");
    }
    std::ifstream in(static_dir_ + "/" + file, std::ios::binary);
    if (!in) {
        return HttpResponse::error(404, "not found");
    }
    std::ostringstream contents;
    contents << in.rdbuf();

    std::string type = "application/octet-stream";
    const auto ends_with = [&file](std::string_view suffix) {
        return file.size() >= suffix.size() &&
               file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (ends_with(".html")) {
        type = "text/html; charset=utf-8";
    } else if (ends_with(".css")) {
        type = "text/css";
    } else if (ends_with(".js")) {
        type = "text/javascript";
    } else if (ends_with(".svg")) {
        type = "image/svg+xml";
    } else if (ends_with(".ico")) {
        type = "image/x-icon";
    }
    return HttpResponse::text(contents.str(), 200, std::move(type));
}

}  // namespace chronos::api
