// Lock-free metrics: counters, log-linear latency histograms, throughput meters.
// Recording is wait-free (relaxed atomics) so it can sit on the data path.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

#include <nlohmann/json_fwd.hpp>

namespace pf {

class Counter {
public:
    void add(std::uint64_t n = 1) noexcept { v_.fetch_add(n, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t get() const noexcept { return v_.load(std::memory_order_relaxed); }
    void reset() noexcept { v_.store(0, std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> v_{0};
};

struct LatencySnapshot {
    std::uint64_t count = 0;
    double mean_us = 0, min_us = 0, max_us = 0, p50_us = 0, p95_us = 0, p99_us = 0;
};

// Log-linear histogram of microsecond values: 8 sub-buckets per power of two,
// relative error <= 12.5%. Range 0 .. ~2^40 us.
class LatencyHistogram {
public:
    static constexpr int kSubBits = 3;
    static constexpr int kSub = 1 << kSubBits;
    static constexpr int kBuckets = 41 * kSub;

    void record_us(std::uint64_t us) noexcept;
    void record_ns(std::uint64_t ns) noexcept { record_us(ns / 1000); }
    [[nodiscard]] LatencySnapshot snapshot() const noexcept;
    void reset() noexcept;

    static int bucket_of(std::uint64_t v) noexcept;
    static std::uint64_t bucket_upper(int b) noexcept;  // inclusive upper bound value

private:
    std::array<std::atomic<std::uint64_t>, kBuckets> buckets_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> sum_{0};
    std::atomic<std::uint64_t> min_{UINT64_MAX};
    std::atomic<std::uint64_t> max_{0};
};

// Bytes counter plus a rate computed by periodic sample() calls (EWMA).
class ThroughputMeter {
public:
    void add(std::uint64_t bytes) noexcept { total_.fetch_add(bytes, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t total() const noexcept { return total_.load(std::memory_order_relaxed); }
    // Call periodically (e.g. 1 Hz) with a monotonic timestamp; returns bytes/s.
    double sample(std::uint64_t now_ns);
    [[nodiscard]] double rate() const noexcept { return rate_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> total_{0};
    std::atomic<double> rate_{0.0};
    std::mutex m_;
    std::uint64_t last_total_ = 0;
    std::uint64_t last_ns_ = 0;
};

// Metrics of one network link (connection).
struct LinkMetrics {
    ThroughputMeter tx;  // upload (bytes written)
    ThroughputMeter rx;  // download (bytes read)
    Counter frames_tx, frames_rx;
    Counter pings_sent, pongs_received;
    LatencyHistogram rtt;
    std::atomic<std::uint64_t> last_rtt_us{0};
    Counter reconnects;
    Counter protocol_errors;

    [[nodiscard]] double packet_loss() const noexcept;  // fraction of unanswered pings
    void sample(std::uint64_t now_ns) { tx.sample(now_ns); rx.sample(now_ns); }
};

// Metrics of USB traffic (per session and aggregated).
struct UsbMetrics {
    Counter transfers, errors, timeouts, stalls, cancels, resets;
    ThroughputMeter bytes_in;   // device -> host
    ThroughputMeter bytes_out;  // host -> device
    LatencyHistogram latency;   // submit -> complete
    void sample(std::uint64_t now_ns) { bytes_in.sample(now_ns); bytes_out.sample(now_ns); }
};

nlohmann::json to_json(const LatencySnapshot& s);
nlohmann::json to_json(const LinkMetrics& m);
nlohmann::json to_json(const UsbMetrics& m);

}  // namespace pf
