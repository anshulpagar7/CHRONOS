// End-to-end API test: the full system (scheduler thread, worker threads,
// metrics, timeline) behind the real HTTP server on an ephemeral port,
// exercised by a raw TCP client. If this passes, `curl` works.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "chronos/api/api_server.h"
#include "chronos/core/event_bus.h"
#include "chronos/core/job_store.h"
#include "chronos/execution/local_thread_backend.h"
#include "chronos/observability/scheduler_metrics.h"
#include "chronos/scheduling/policies.h"

namespace chronos {
namespace {

using namespace std::chrono_literals;
using api::JsonValue;

/// Minimal blocking HTTP client for tests.
std::string http_request(std::uint16_t port, const std::string& method,
                         const std::string& target, const std::string& body = "") {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    std::string request = method + " " + target + " HTTP/1.1\r\n" +
                          "Host: localhost\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body;
    EXPECT_EQ(::send(fd, request.data(), request.size(), 0),
              static_cast<ssize_t>(request.size()));

    std::string response;
    char chunk[4096];
    ssize_t n = 0;
    while ((n = ::recv(fd, chunk, sizeof(chunk), 0)) > 0) {
        response.append(chunk, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return response;
}

std::string body_of(const std::string& response) {
    const std::size_t split = response.find("\r\n\r\n");
    return split == std::string::npos ? "" : response.substr(split + 4);
}

int status_of(const std::string& response) {
    return std::atoi(response.substr(9, 3).c_str());
}

class ApiIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        scheduler_ = std::make_unique<Scheduler>(
            store_, registry_, bus_, clock_, std::make_unique<CompositePolicy>(),
            RetryConfig{.base_backoff = 10ms, .jitter = 0.0});
        backend_ = std::make_unique<LocalThreadBackend>(
            registry_, *scheduler_, clock_,
            [](const Job& job) {
                std::this_thread::sleep_for(5ms);
                return ExecutionResult{.success = job.spec.payload != "fail",
                                       .error = {}};
            },
            std::vector<ResourceCapacity>{{.cpu_units = 4, .memory_mb = 1024}},
            /*heartbeat_interval=*/50ms);
        scheduler_->attach_backend(*backend_);
        backend_->start();
        scheduler_->start();

        server_ = std::make_unique<api::ApiServer>(*scheduler_, store_, registry_,
                                                   metrics_, timeline_, clock_,
                                                   /*port=*/0);
        server_->start();
        port_ = server_->port();
    }

    void TearDown() override {
        server_->stop();
        scheduler_->stop(/*drain=*/false);
        backend_->stop();
    }

    /// Poll the API until `job_id` reports `state` (or time out).
    JsonValue wait_for_state(std::uint64_t job_id, const std::string& state) {
        for (int i = 0; i < 200; ++i) {
            const JsonValue job = JsonValue::parse(body_of(http_request(
                port_, "GET", "/api/jobs/" + std::to_string(job_id))));
            if (job.at("state").as_string() == state) {
                return job;
            }
            std::this_thread::sleep_for(10ms);
        }
        ADD_FAILURE() << "job " << job_id << " never reached " << state;
        return JsonValue();
    }

    SystemClock clock_;
    EventBus bus_;
    JobStore store_{clock_, bus_};
    WorkerRegistry registry_{clock_, bus_};
    MetricsRegistry metrics_;
    SchedulerMetrics scheduler_metrics_{bus_, metrics_};
    TimelineRecorder timeline_{bus_};
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<LocalThreadBackend> backend_;
    std::unique_ptr<api::ApiServer> server_;
    std::uint16_t port_ = 0;
};

TEST_F(ApiIntegrationTest, SubmitRunsToCompletionAndTimelineShowsIt) {
    const std::string response = http_request(
        port_, "POST", "/api/jobs",
        R"({"name":"encode-video","priority":7,"cpu":2,"memory_mb":128})");
    EXPECT_EQ(status_of(response), 201);
    const JsonValue created = JsonValue::parse(body_of(response));
    const auto id = static_cast<std::uint64_t>(created.at("id").as_number());
    EXPECT_GE(id, 1u);

    const JsonValue job = wait_for_state(id, "COMPLETED");
    EXPECT_EQ(job.at("name").as_string(), "encode-video");
    EXPECT_EQ(job.at("priority").as_number(), 7);
    EXPECT_EQ(job.at("attempt").as_number(), 1);

    // Timeline captures the canonical path.
    const auto& timeline = job.at("timeline").as_array();
    ASSERT_GE(timeline.size(), 4u);
    EXPECT_EQ(timeline.front().at("to").as_string(), "QUEUED");
    EXPECT_EQ(timeline.back().at("to").as_string(), "COMPLETED");
}

TEST_F(ApiIntegrationTest, StateEndpointReflectsClusterAndCounts) {
    http_request(port_, "POST", "/api/jobs", R"({"name":"a"})");
    const JsonValue job_b = JsonValue::parse(
        body_of(http_request(port_, "POST", "/api/jobs", R"({"name":"b"})")));
    wait_for_state(static_cast<std::uint64_t>(job_b.at("id").as_number()),
                   "COMPLETED");

    const JsonValue state =
        JsonValue::parse(body_of(http_request(port_, "GET", "/api/state")));
    EXPECT_EQ(state.at("workers").size(), 1u);
    const JsonValue& worker = state.at("workers").as_array()[0];
    EXPECT_TRUE(worker.at("alive").as_bool());
    EXPECT_EQ(worker.at("cpu_total").as_number(), 4);
    EXPECT_TRUE(state.at("reservation").is_null());
    EXPECT_GE(state.at("job_counts").at("COMPLETED").as_number(), 1);
}

TEST_F(ApiIntegrationTest, ListFiltersByStateAndRespectsLimit) {
    JsonValue last;
    for (int i = 0; i < 5; ++i) {
        last = JsonValue::parse(body_of(
            http_request(port_, "POST", "/api/jobs", R"({"name":"batch"})")));
    }
    wait_for_state(static_cast<std::uint64_t>(last.at("id").as_number()),
                   "COMPLETED");

    const JsonValue limited = JsonValue::parse(
        body_of(http_request(port_, "GET", "/api/jobs?limit=3")));
    EXPECT_EQ(limited.at("jobs").size(), 3u);

    const JsonValue completed = JsonValue::parse(body_of(
        http_request(port_, "GET", "/api/jobs?state=COMPLETED&limit=500")));
    for (const JsonValue& job : completed.at("jobs").as_array()) {
        EXPECT_EQ(job.at("state").as_string(), "COMPLETED");
    }
}

TEST_F(ApiIntegrationTest, FailingJobRetriesAndDeadLetters) {
    const JsonValue created = JsonValue::parse(body_of(http_request(
        port_, "POST", "/api/jobs",
        R"({"name":"doomed","max_retries":1,"payload":"fail"})")));
    const auto id = static_cast<std::uint64_t>(created.at("id").as_number());

    const JsonValue job = wait_for_state(id, "FAILED");
    EXPECT_EQ(job.at("attempt").as_number(), 2);  // Original + one retry.

    // The metrics endpoint saw it all.
    const std::string metrics =
        body_of(http_request(port_, "GET", "/metrics"));
    EXPECT_NE(metrics.find("chronos_retries_total 1"), std::string::npos);
    EXPECT_NE(metrics.find("chronos_jobs_failed_total 1"), std::string::npos);
}

TEST_F(ApiIntegrationTest, CancelAndErrorPaths) {
    // Cancelling a nonexistent job: 404.
    EXPECT_EQ(status_of(http_request(port_, "POST", "/api/jobs/999/cancel")), 404);
    // Unknown route: 404. Malformed JSON: 400. Bad spec: 400.
    EXPECT_EQ(status_of(http_request(port_, "GET", "/api/nope")), 404);
    EXPECT_EQ(status_of(http_request(port_, "POST", "/api/jobs", "{oops")), 400);
    EXPECT_EQ(status_of(http_request(port_, "POST", "/api/jobs",
                                     R"({"priority":99})")), 400);

    // Events endpoint returns a well-formed feed.
    http_request(port_, "POST", "/api/jobs", R"({"name":"observed"})");
    const JsonValue events = JsonValue::parse(
        body_of(http_request(port_, "GET", "/api/events?limit=10")));
    EXPECT_GE(events.at("events").size(), 1u);
}

TEST_F(ApiIntegrationTest, ConcurrentClientsSeeConsistentResponses) {
    // Hammer the server from several threads with mixed reads and writes.
    // Every response must be well-formed; every submit must be honoured.
    constexpr int kThreads = 6;
    constexpr int kPerThread = 25;
    std::atomic<int> submits{0};
    std::atomic<int> bad{0};

    std::vector<std::thread> clients;
    for (int t = 0; t < kThreads; ++t) {
        clients.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                std::string response;
                switch ((t + i) % 4) {
                    case 0:
                        response = http_request(port_, "POST", "/api/jobs",
                                                R"({"name":"swarm"})");
                        if (status_of(response) == 201) {
                            submits.fetch_add(1);
                        } else {
                            bad.fetch_add(1);
                        }
                        break;
                    case 1: response = http_request(port_, "GET", "/api/state"); break;
                    case 2: response = http_request(port_, "GET", "/api/jobs?limit=10"); break;
                    case 3: response = http_request(port_, "GET", "/metrics"); break;
                }
                if (response.find("HTTP/1.1") != 0) {
                    bad.fetch_add(1);
                }
                if ((t + i) % 4 != 3) {  // JSON endpoints must parse.
                    try {
                        (void)JsonValue::parse(body_of(response));
                    } catch (...) {
                        bad.fetch_add(1);
                    }
                }
            }
        });
    }
    for (auto& c : clients) {
        c.join();
    }

    EXPECT_EQ(bad.load(), 0);
    EXPECT_GT(submits.load(), 0);
    // What matters is agreement with the system's own view:
    const JsonValue state =
        JsonValue::parse(body_of(http_request(port_, "GET", "/api/state")));
    double total = 0;
    for (const auto& [name, count] : state.at("job_counts").as_object()) {
        total += count.as_number();
    }
    EXPECT_GE(static_cast<int>(total), submits.load());
}

}  // namespace
}  // namespace chronos
