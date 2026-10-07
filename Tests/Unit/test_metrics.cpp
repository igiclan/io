#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "Diagnostics/Metrics.hpp"

using namespace pf;

TEST_CASE("histogram buckets are monotonic and cover their values") {
    for (std::uint64_t v : {0ull, 1ull, 7ull, 8ull, 9ull, 15ull, 16ull, 17ull, 100ull, 1000ull, 123456ull, 999999999ull}) {
        int b = LatencyHistogram::bucket_of(v);
        CHECK(LatencyHistogram::bucket_upper(b) >= v);
        if (b > 0) CHECK(LatencyHistogram::bucket_upper(b - 1) < v);
    }
}

TEST_CASE("percentiles are within histogram precision") {
    LatencyHistogram h;
    for (int i = 1; i <= 1000; ++i) h.record_us(static_cast<std::uint64_t>(i));
    auto s = h.snapshot();
    CHECK(s.count == 1000);
    CHECK(s.mean_us == doctest::Approx(500.5));
    CHECK(s.min_us == 1);
    CHECK(s.max_us == 1000);
    CHECK(s.p50_us == doctest::Approx(500).epsilon(0.13));
    CHECK(s.p95_us == doctest::Approx(950).epsilon(0.13));
    CHECK(s.p99_us == doctest::Approx(990).epsilon(0.13));
    h.reset();
    CHECK(h.snapshot().count == 0);
}

TEST_CASE("throughput meter") {
    ThroughputMeter t;
    t.sample(1'000'000'000);
    t.add(1'000'000);
    double r = t.sample(2'000'000'000);
    CHECK(r == doctest::Approx(1'000'000));
    CHECK(t.total() == 1'000'000);
}

TEST_CASE("packet loss accounting") {
    LinkMetrics m;
    for (int i = 0; i < 11; ++i) m.pings_sent.add();
    for (int i = 0; i < 9; ++i) m.pongs_received.add();
    CHECK(m.packet_loss() == doctest::Approx(0.1));
    auto j = to_json(m);
    CHECK(j.contains("rtt"));
}
