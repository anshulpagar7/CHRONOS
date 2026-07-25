#include "chronos/observability/metrics.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

namespace chronos {
namespace {

TEST(Metrics, CounterIncrements) {
    Counter c;
    EXPECT_EQ(c.value(), 0u);
    c.inc();
    c.inc(41);
    EXPECT_EQ(c.value(), 42u);
}

TEST(Metrics, GaugeMovesBothWays) {
    Gauge g;
    g.set(10);
    g.add(5);
    g.sub(3);
    EXPECT_EQ(g.value(), 12);
    g.sub(20);
    EXPECT_EQ(g.value(), -8);
}

TEST(Metrics, HistogramCountsSumsAndBuckets) {
    Histogram h({1.0, 5.0, 10.0});
    h.record(0.5);   // bucket <=1
    h.record(1.0);   // bucket <=1 (boundary is inclusive)
    h.record(3.0);   // bucket <=5
    h.record(7.0);   // bucket <=10
    h.record(100.0); // +Inf

    EXPECT_EQ(h.count(), 5u);
    EXPECT_DOUBLE_EQ(h.sum(), 111.5);
    const auto buckets = h.bucket_counts();
    ASSERT_EQ(buckets.size(), 4u);
    EXPECT_EQ(buckets[0], 2u);
    EXPECT_EQ(buckets[1], 1u);
    EXPECT_EQ(buckets[2], 1u);
    EXPECT_EQ(buckets[3], 1u);  // +Inf
}

TEST(Metrics, HistogramPercentileInterpolatesWithinBucket) {
    Histogram h({10.0, 20.0});
    for (int i = 0; i < 100; ++i) {
        h.record(15.0);  // All samples in the (10, 20] bucket.
    }
    // Any percentile lands in that bucket; interpolation stays within it.
    EXPECT_GE(h.percentile(0.5), 10.0);
    EXPECT_LE(h.percentile(0.5), 20.0);
    EXPECT_GE(h.percentile(0.99), h.percentile(0.01));

    Histogram empty({1.0});
    EXPECT_DOUBLE_EQ(empty.percentile(0.99), 0.0);
}

TEST(Metrics, HistogramPercentileClampsInfBucketToLastBoundary) {
    Histogram h({1.0});
    h.record(1e9);
    EXPECT_DOUBLE_EQ(h.percentile(0.99), 1.0);
}

TEST(Metrics, HistogramRejectsBadBoundaries) {
    EXPECT_THROW(Histogram({}), std::invalid_argument);
    EXPECT_THROW(Histogram({5.0, 1.0}), std::invalid_argument);
}

TEST(Metrics, RegistryGetOrCreateReturnsSameInstance) {
    MetricsRegistry registry;
    Counter& a = registry.counter("x_total", "help");
    Counter& b = registry.counter("x_total", "other help");
    a.inc();
    EXPECT_EQ(&a, &b);
    EXPECT_EQ(b.value(), 1u);
}

TEST(Metrics, PrometheusRenderContainsWellFormedExposition) {
    MetricsRegistry registry;
    registry.counter("chronos_test_total", "a counter").inc(3);
    registry.gauge("chronos_depth", "a gauge").set(7);
    auto& h = registry.histogram("chronos_lat_seconds", "a histogram", {0.1, 1.0});
    h.record(0.05);
    h.record(0.5);
    h.record(5.0);

    const std::string out = registry.render_prometheus();

    EXPECT_NE(out.find("# TYPE chronos_test_total counter\nchronos_test_total 3\n"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE chronos_depth gauge\nchronos_depth 7\n"),
              std::string::npos);
    // Histogram buckets are cumulative.
    EXPECT_NE(out.find("chronos_lat_seconds_bucket{le=\"0.1\"} 1"), std::string::npos);
    EXPECT_NE(out.find("chronos_lat_seconds_bucket{le=\"1\"} 2"), std::string::npos);
    EXPECT_NE(out.find("chronos_lat_seconds_bucket{le=\"+Inf\"} 3"), std::string::npos);
    EXPECT_NE(out.find("chronos_lat_seconds_count 3"), std::string::npos);
}

TEST(Metrics, ConcurrentRecordingLosesNothing) {
    MetricsRegistry registry;
    Counter& c = registry.counter("c_total", "");
    Histogram& h = registry.histogram("h_seconds", "", {1.0});

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                c.inc();
                h.record(0.5);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(c.value(), 4000u);
    EXPECT_EQ(h.count(), 4000u);
}

}  // namespace
}  // namespace chronos
