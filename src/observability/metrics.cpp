#include "chronos/observability/metrics.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace chronos {

namespace {

std::string format_double(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

std::vector<double> Histogram::default_boundaries() {
    // Seconds, roughly x2..x2.5 exponential: 1ms .. 60s.
    return {0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1,
            0.25,  0.5,    1.0,   2.5,  5.0,   10.0, 30.0, 60.0};
}

Histogram::Histogram(std::vector<double> boundaries)
    : boundaries_(std::move(boundaries)), buckets_(boundaries_.size() + 1, 0) {
    if (!std::is_sorted(boundaries_.begin(), boundaries_.end())) {
        throw std::invalid_argument("Histogram: boundaries must be ascending");
    }
    if (boundaries_.empty()) {
        throw std::invalid_argument("Histogram: at least one boundary required");
    }
}

void Histogram::record(double value) {
    const auto it = std::lower_bound(boundaries_.begin(), boundaries_.end(), value);
    const std::size_t idx = static_cast<std::size_t>(it - boundaries_.begin());
    std::lock_guard lock(mutex_);
    ++buckets_[idx];
    ++count_;
    sum_ += value;
}

std::uint64_t Histogram::count() const {
    std::lock_guard lock(mutex_);
    return count_;
}

double Histogram::sum() const {
    std::lock_guard lock(mutex_);
    return sum_;
}

std::vector<std::uint64_t> Histogram::bucket_counts() const {
    std::lock_guard lock(mutex_);
    return buckets_;
}

double Histogram::percentile(double p) const {
    p = std::clamp(p, 0.0, 1.0);
    std::lock_guard lock(mutex_);
    if (count_ == 0) {
        return 0.0;
    }
    const double target = p * static_cast<double>(count_);
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
        const std::uint64_t in_bucket = buckets_[i];
        if (static_cast<double>(cumulative + in_bucket) < target || in_bucket == 0) {
            cumulative += in_bucket;
            continue;
        }
        // Interpolate within bucket i.
        const double lower = (i == 0) ? 0.0 : boundaries_[i - 1];
        if (i >= boundaries_.size()) {
            return boundaries_.back();  // +Inf bucket: clamp.
        }
        const double upper = boundaries_[i];
        const double into = (target - static_cast<double>(cumulative)) /
                            static_cast<double>(in_bucket);
        return lower + (upper - lower) * std::clamp(into, 0.0, 1.0);
    }
    return boundaries_.back();
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

Counter& MetricsRegistry::counter(const std::string& name, const std::string& help) {
    std::lock_guard lock(mutex_);
    auto it = counters_.find(name);
    if (it == counters_.end()) {
        it = counters_.emplace(name, Entry<Counter>{help, std::make_unique<Counter>()})
                 .first;
    }
    return *it->second.metric;
}

Gauge& MetricsRegistry::gauge(const std::string& name, const std::string& help) {
    std::lock_guard lock(mutex_);
    auto it = gauges_.find(name);
    if (it == gauges_.end()) {
        it = gauges_.emplace(name, Entry<Gauge>{help, std::make_unique<Gauge>()}).first;
    }
    return *it->second.metric;
}

Histogram& MetricsRegistry::histogram(const std::string& name, const std::string& help,
                                      std::vector<double> boundaries) {
    std::lock_guard lock(mutex_);
    auto it = histograms_.find(name);
    if (it == histograms_.end()) {
        it = histograms_
                 .emplace(name, Entry<Histogram>{
                                    help, std::make_unique<Histogram>(std::move(boundaries))})
                 .first;
    }
    return *it->second.metric;
}

std::string MetricsRegistry::render_prometheus() const {
    std::lock_guard lock(mutex_);
    std::string out;
    out.reserve(4096);

    for (const auto& [name, entry] : counters_) {
        out += "# HELP " + name + " " + entry.help + "\n";
        out += "# TYPE " + name + " counter\n";
        out += name + " " + std::to_string(entry.metric->value()) + "\n";
    }
    for (const auto& [name, entry] : gauges_) {
        out += "# HELP " + name + " " + entry.help + "\n";
        out += "# TYPE " + name + " gauge\n";
        out += name + " " + std::to_string(entry.metric->value()) + "\n";
    }
    for (const auto& [name, entry] : histograms_) {
        out += "# HELP " + name + " " + entry.help + "\n";
        out += "# TYPE " + name + " histogram\n";
        const auto& bounds = entry.metric->boundaries();
        const auto buckets = entry.metric->bucket_counts();
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < bounds.size(); ++i) {
            cumulative += buckets[i];
            out += name + "_bucket{le=\"" + format_double(bounds[i]) + "\"} " +
                   std::to_string(cumulative) + "\n";
        }
        cumulative += buckets.back();
        out += name + "_bucket{le=\"+Inf\"} " + std::to_string(cumulative) + "\n";
        out += name + "_sum " + format_double(entry.metric->sum()) + "\n";
        out += name + "_count " + std::to_string(entry.metric->count()) + "\n";
    }
    return out;
}

}  // namespace chronos
