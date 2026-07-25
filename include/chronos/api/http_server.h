#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "chronos/api/json.h"

namespace chronos::api {

struct HttpRequest {
    std::string method;                        ///< Uppercased.
    std::string path;                          ///< Decoded path, no query.
    std::map<std::string, std::string> query;  ///< Decoded query parameters.
    std::map<std::string, std::string> headers;      ///< Keys lowercased.
    std::map<std::string, std::string> path_params;  ///< From {name} segments.
    std::string body;

    [[nodiscard]] std::optional<std::string> query_param(const std::string& key) const {
        const auto it = query.find(key);
        if (it == query.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;

    static HttpResponse json(const JsonValue& v, int status = 200) {
        return {status, "application/json", v.dump()};
    }
    static HttpResponse text(std::string body, int status = 200,
                             std::string content_type = "text/plain; charset=utf-8") {
        return {status, std::move(content_type), std::move(body)};
    }
    static HttpResponse error(int status, const std::string& message) {
        return json(JsonValue::object().set("error", message), status);
    }
};

namespace detail {
/// Parse one HTTP/1.1 request (start line + headers + body, already fully
/// buffered). Returns nullopt on malformed input. Exposed for unit tests.
std::optional<HttpRequest> parse_request(std::string_view raw);

/// %XX and '+' decoding for path/query components.
std::string url_decode(std::string_view s);
}  // namespace detail

/// Method+pattern routing. Patterns are literal segments or `{name}`
/// captures: "/api/jobs/{id}" matches "/api/jobs/42" with
/// path_params["id"] == "42". First registered match wins.
class Router {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    void add(std::string method, std::string pattern, Handler handler);

    /// Route a parsed request. nullopt when nothing matches (404 is the
    /// caller's decision -- the server falls through to static files).
    [[nodiscard]] std::optional<HttpResponse> route(HttpRequest& request) const;

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments;  // "{name}" entries capture.
        Handler handler;
    };
    std::vector<Route> routes_;
};

/// Deliberately small HTTP/1.1 server (ADR-010): blocking accept loop
/// feeding a fixed worker pool, one request per connection
/// (Connection: close). No TLS, no keep-alive, no chunked encoding --
/// none of which a localhost dashboard/API daemon needs. Hard limits on
/// header and body size guard against hostile input.
class HttpServer {
public:
    /// `port` 0 binds an ephemeral port (see port()). Not started yet.
    HttpServer(Router router, std::uint16_t port, int worker_threads = 4);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// Bind, listen, spin up threads. Throws std::runtime_error on bind
    /// failure (e.g. port already in use).
    void start();
    void stop();

    /// The actually-bound port (useful after binding port 0).
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    void accept_loop();
    void worker_loop();
    void handle_connection(int fd);

    Router router_;
    std::uint16_t port_;
    int worker_threads_;

    // Atomic: written by stop() while accept_loop() reads it. stop()
    // flips running_ first, so the accept error path after close() exits.
    std::atomic<int> listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread acceptor_;
    std::vector<std::thread> workers_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<int> pending_fds_;
};

}  // namespace chronos::api
