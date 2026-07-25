#include "chronos/api/http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "chronos/observability/log.h"

namespace chronos::api {

namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;

std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start < path.size()) {
        if (path[start] == '/') {
            ++start;
            continue;
        }
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        segments.emplace_back(path.substr(start, end - start));
        start = end;
    }
    return segments;
}

/// ::send() may write fewer bytes than asked; loop until done or error.
bool send_all(int fd, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "";
    }
}

}  // namespace

namespace detail {

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
            } else {
                out += s[i];
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

std::optional<HttpRequest> parse_request(std::string_view raw) {
    const std::size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return std::nullopt;
    }

    HttpRequest request;
    std::size_t line_start = 0;

    // Start line: METHOD SP TARGET SP VERSION
    {
        const std::size_t line_end = raw.find("\r\n", line_start);
        const std::string_view line = raw.substr(line_start, line_end - line_start);
        const std::size_t sp1 = line.find(' ');
        const std::size_t sp2 = line.rfind(' ');
        if (sp1 == std::string_view::npos || sp2 == sp1) {
            return std::nullopt;
        }
        request.method = std::string(line.substr(0, sp1));
        std::transform(request.method.begin(), request.method.end(),
                       request.method.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::size_t q = target.find('?');
        request.path = url_decode(target.substr(0, q));
        if (q != std::string_view::npos) {
            std::string_view qs = target.substr(q + 1);
            while (!qs.empty()) {
                std::size_t amp = qs.find('&');
                const std::string_view pair = qs.substr(0, amp);
                const std::size_t eq = pair.find('=');
                if (eq != std::string_view::npos) {
                    request.query[url_decode(pair.substr(0, eq))] =
                        url_decode(pair.substr(eq + 1));
                } else if (!pair.empty()) {
                    request.query[url_decode(pair)] = "";
                }
                if (amp == std::string_view::npos) {
                    break;
                }
                qs = qs.substr(amp + 1);
            }
        }
        line_start = line_end + 2;
    }

    // Headers.
    while (line_start < header_end) {
        const std::size_t line_end = raw.find("\r\n", line_start);
        const std::string_view line = raw.substr(line_start, line_end - line_start);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string key(line.substr(0, colon));
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            request.headers[key] = std::string(value);
        }
        line_start = line_end + 2;
    }

    request.body = std::string(raw.substr(header_end + 4));
    return request;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

void Router::add(std::string method, std::string pattern, Handler handler) {
    routes_.push_back(
        Route{std::move(method), split_path(pattern), std::move(handler)});
}

std::optional<HttpResponse> Router::route(HttpRequest& request) const {
    const std::vector<std::string> segments = split_path(request.path);
    for (const Route& route : routes_) {
        if (route.method != request.method ||
            route.segments.size() != segments.size()) {
            continue;
        }
        std::map<std::string, std::string> params;
        bool match = true;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const std::string& pattern_seg = route.segments[i];
            if (pattern_seg.size() >= 2 && pattern_seg.front() == '{' &&
                pattern_seg.back() == '}') {
                params[pattern_seg.substr(1, pattern_seg.size() - 2)] = segments[i];
            } else if (pattern_seg != segments[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            request.path_params = std::move(params);
            return route.handler(request);
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

HttpServer::HttpServer(Router router, std::uint16_t port, int worker_threads)
    : router_(std::move(router)), port_(port), worker_threads_(worker_threads) {}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("HttpServer: socket() failed");
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("HttpServer: bind() failed on port " +
                                 std::to_string(port_));
    }
    if (port_ == 0) {  // Recover the ephemeral port.
        socklen_t len = sizeof(addr);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }
    if (::listen(fd, 64) < 0) {
        ::close(fd);
        throw std::runtime_error("HttpServer: listen() failed");
    }

    listen_fd_.store(fd);
    running_.store(true);
    for (int i = 0; i < worker_threads_; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
    acceptor_ = std::thread([this] { accept_loop(); });
    log::info("http server listening", {{"port", std::to_string(port_)}});
}

void HttpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    // Unblock accept() by shutting the listening socket down. running_
    // is already false, so the accept error path exits cleanly.
    if (const int fd = listen_fd_.exchange(-1); fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    queue_cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    // Drain anything still queued.
    std::lock_guard lock(queue_mutex_);
    for (const int fd : pending_fds_) {
        ::close(fd);
    }
    pending_fds_.clear();
}

void HttpServer::accept_loop() {
    while (running_.load()) {
        const int client = ::accept(listen_fd_.load(), nullptr, nullptr);
        if (client < 0) {
            if (running_.load()) {
                // Transient (e.g. EMFILE under load): brief backoff instead
                // of spinning the core.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            break;  // stop() closed the socket.
        }
        {
            std::lock_guard lock(queue_mutex_);
            pending_fds_.push_back(client);
        }
        queue_cv_.notify_one();
    }
}

void HttpServer::worker_loop() {
    while (true) {
        int fd = -1;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock,
                           [this] { return !pending_fds_.empty() || !running_.load(); });
            if (pending_fds_.empty()) {
                return;  // Shutting down.
            }
            fd = pending_fds_.front();
            pending_fds_.pop_front();
        }
        handle_connection(fd);
    }
}

void HttpServer::handle_connection(int fd) {
    // Read until the full head is buffered, then until content-length is met.
    timeval timeout{.tv_sec = 5, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string buffer;
    std::size_t body_needed = std::string::npos;
    char chunk[4096];
    while (true) {
        const std::size_t head_end = buffer.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            if (body_needed == std::string::npos) {
                const auto parsed_head =
                    detail::parse_request(std::string_view(buffer).substr(0, head_end + 4));
                if (!parsed_head) {
                    body_needed = 0;
                    break;
                }
                const auto it = parsed_head->headers.find("content-length");
                body_needed =
                    it == parsed_head->headers.end()
                        ? 0
                        : static_cast<std::size_t>(std::strtoull(it->second.c_str(),
                                                                 nullptr, 10));
                if (body_needed > kMaxBodyBytes) {
                    const std::string payload =
                        HttpResponse::error(413, "body too large").body;
                    std::string head = "HTTP/1.1 413 Payload Too Large\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Content-Length: " + std::to_string(payload.size()) +
                                       "\r\nConnection: close\r\n\r\n";
                    head += payload;
                    send_all(fd, head.data(), head.size());
                    ::close(fd);
                    return;
                }
            }
            if (buffer.size() >= head_end + 4 + body_needed) {
                break;  // Full request buffered.
            }
        } else if (buffer.size() > kMaxHeaderBytes) {
            ::close(fd);
            return;
        }
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            ::close(fd);
            return;  // Timeout, reset, or premature close.
        }
        buffer.append(chunk, static_cast<std::size_t>(n));
    }

    auto request = detail::parse_request(buffer);
    HttpResponse response;
    if (!request) {
        response = HttpResponse::error(400, "malformed request");
    } else {
        try {
            auto routed = router_.route(*request);
            response = routed ? std::move(*routed)
                              : HttpResponse::error(404, "not found");
        } catch (const std::exception& e) {
            response = HttpResponse::error(500, e.what());
        }
    }

    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " +
                      status_text(response.status) + "\r\n" +
                      "Content-Type: " + response.content_type + "\r\n" +
                      "Content-Length: " + std::to_string(response.body.size()) + "\r\n" +
                      "Access-Control-Allow-Origin: *\r\n"
                      "Connection: close\r\n\r\n";
    out += response.body;
    send_all(fd, out.data(), out.size());
    ::close(fd);
}

}  // namespace chronos::api
