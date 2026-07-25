#include "chronos/observability/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace chronos::log {

namespace {

std::atomic<Level> g_level{Level::Info};
std::mutex g_sink_mutex;

/// Wall-clock ISO-8601 UTC timestamp with millisecond precision.
/// (steady_clock drives scheduling; system_clock labels log lines --
/// humans read logs against wall time.)
std::string timestamp_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);

    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);

    char buf[96];  // Sized for snprintf worst-case analysis under -Wformat-truncation.
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}

bool needs_quoting(const std::string& value) {
    if (value.empty()) {
        return true;
    }
    for (const char c : value) {
        if (c == ' ' || c == '"' || c == '=' || c == '\n' || c == '\t') {
            return true;
        }
    }
    return false;
}

void append_value(std::string& out, const std::string& value) {
    if (!needs_quoting(value)) {
        out += value;
        return;
    }
    out += '"';
    for (const char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    out += '"';
}

}  // namespace

const char* to_string(Level level) noexcept {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

void set_level(Level level) noexcept {
    g_level.store(level, std::memory_order_relaxed);
}

Level level() noexcept {
    return g_level.load(std::memory_order_relaxed);
}

void write(Level lvl, std::string_view message, std::initializer_list<Field> fields) {
    if (lvl < g_level.load(std::memory_order_relaxed)) {
        return;
    }

    // Build the full line first, emit under the sink lock in one call --
    // lines from concurrent threads never interleave mid-line.
    std::string line;
    line.reserve(96 + message.size());
    line += timestamp_now();
    line += ' ';
    line += to_string(lvl);
    line += ' ';
    line += message;
    for (const auto& field : fields) {
        line += ' ';
        line += field.key;
        line += '=';
        append_value(line, field.value);
    }
    line += '\n';

    std::lock_guard lock(g_sink_mutex);
    std::fwrite(line.data(), 1, line.size(), stdout);
}

}  // namespace chronos::log
