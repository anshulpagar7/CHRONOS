#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace chronos {

/// Monotonically increasing count (events, totals).
class Counter {
public:
    void inc(std::uint64_t n = 1) noexcept { value_.fetch_add(n, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> value_{0};
};

/// Point-in-time level (queue depth, running jobs). May go up and down.
class Gauge {
public:
    void set(std::int64_t v) noexcept { value_.store(v, std::memory_order_relaxed); }
    void add(std::int64_t n = 1) noexcept { value_.fetch_add(n, std::memory_order_relaxed); }
    void sub(std::int64_t n = 1) noexcept { value_.fetch_sub(n, std::memory_order_relaxed); }
    [[nodiscard]] std::int64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::int64_t> value_{0};
};

/// Bucketed histogram, Prometheus-style: fixed upper boundaries, a count
/// per bucket, plus total sum and count. Chosen over raw-sample storage
/// because it is O(1) memory forever, O(log b) to record, renders natively
/// to the Prometheus exposition format, and percentile estimation by
/// linear interpolation within a bucket is plenty for operational use
/// (exact assertions in tests use count()/sum(), which are precise).
class Histogram {
public:
    /// `boundaries` are ascending bucket upper bounds; an implicit +Inf
    /// bucket is appended. Default: exponential seconds 1ms .. 60s.
    explicit Histogram(std::vector<double> boundaries = default_boundaries());

    void record(double value);

    [[nodiscard]] std::uint64_t count() const;
    [[nodiscard]] double sum() const;

    /// Estimated p-quantile (p in [0,1]), linearly interpolated within the
    /// containing bucket. Returns 0 for an empty histogram; values in the
    /// +Inf bucket clamp to the last finite boundary.
    [[nodiscard]] double percentile(double p) const;

    [[nodiscard]] const std::vector<double>& boundaries() const noexcept {
        return boundaries_;
    }
    /// Per-bucket counts, including the trailing +Inf bucket.
    [[nodiscard]] std::vector<std::uint64_t> bucket_counts() const;

    [[nodiscard]] static std::vector<double> default_boundaries();

private:
    std::vector<double> boundaries_;
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> buckets_;  // boundaries_.size() + 1 (+Inf).
    std::uint64_t count_ = 0;
    double sum_ = 0.0;
};

/// Named metric registry with Prometheus text exposition.
///
/// get-or-create semantics: the first call defines the metric; later calls
/// with the same name return the same instance (help text of the first
/// wins). Returned references live as long as the registry.
class MetricsRegistry {
public:
    Counter& counter(const std::string& name, const std::string& help);
    Gauge& gauge(const std::string& name, const std::string& help);
    Histogram& histogram(const std::string& name, const std::string& help,
                         std::vector<double> boundaries = Histogram::default_boundaries());

    /// Render every metric in the Prometheus text exposition format
    /// (verbatim scrape-able; the Phase-5 /metrics endpoint returns this).
    [[nodiscard]] std::string render_prometheus() const;

private:
    template <typename T>
    struct Entry {
        std::string help;
        std::unique_ptr<T> metric;
    };

    mutable std::mutex mutex_;
    // std::map: stable iteration order -> stable exposition output.
    std::map<std::string, Entry<Counter>> counters_;
    std::map<std::string, Entry<Gauge>> gauges_;
    std::map<std::string, Entry<Histogram>> histograms_;
};

}  // namespace chronos
