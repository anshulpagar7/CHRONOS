#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace chronos::log {

enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error };

[[nodiscard]] const char* to_string(Level level) noexcept;

/// A single key=value pair attached to a log line.
struct Field {
    std::string_view key;
    std::string value;

    Field(std::string_view k, std::string v) : key(k), value(std::move(v)) {}
    Field(std::string_view k, const char* v) : key(k), value(v) {}
    Field(std::string_view k, std::uint64_t v) : key(k), value(std::to_string(v)) {}
    Field(std::string_view k, std::int64_t v) : key(k), value(std::to_string(v)) {}
    Field(std::string_view k, int v) : key(k), value(std::to_string(v)) {}
    Field(std::string_view k, double v) : key(k), value(std::to_string(v)) {}
};

/// Minimum level that will be emitted. Default: Info.
void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;

/// Emit one structured line:
///   2026-07-08T10:15:03.412Z INFO  job submitted job_id=42 priority=5
///
/// Thread-safe (single global sink mutex). Intentionally dependency-free:
/// the CHRONOS core links against nothing but the standard library.
/// Machine-parseable key=value output means `grep job_id=42` reconstructs
/// one job's whole story from the log.
void write(Level level, std::string_view message, std::initializer_list<Field> fields = {});

inline void trace(std::string_view msg, std::initializer_list<Field> f = {}) { write(Level::Trace, msg, f); }
inline void debug(std::string_view msg, std::initializer_list<Field> f = {}) { write(Level::Debug, msg, f); }
inline void info(std::string_view msg, std::initializer_list<Field> f = {})  { write(Level::Info, msg, f); }
inline void warn(std::string_view msg, std::initializer_list<Field> f = {})  { write(Level::Warn, msg, f); }
inline void error(std::string_view msg, std::initializer_list<Field> f = {}) { write(Level::Error, msg, f); }

}  // namespace chronos::log
