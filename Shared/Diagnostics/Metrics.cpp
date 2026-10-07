#include "Diagnostics/Metrics.hpp"

#include <bit>
#include <nlohmann/json.hpp>

namespace pf {

int LatencyHistogram::bucket_of(std::uint64_t v) noexcept {
    if (v < kSub) return static_cast<int>(v);
    const int msb = 63 - std::countl_zero(v);          // >= kSubBits
    const int shift = msb - kSubBits;
    const int sub = static_cast<int>((v >> shift) & (kSub - 1));
    const int b = (shift + 1) * kSub + sub;
    return b < kBuckets ? b : kBuckets - 1;
}

std::uint64_t LatencyHistogram::bucket_upper(int b) noexcept {
    if (b < kSub) return static_cast<std::uint64_t>(b);
    const int shift = b / kSub - 1;
    const std::uint64_t sub = static_cast<std::uint64_t>(b % kSub);
    return ((static_cast<std::uint64_t>(kSub) + sub + 1) << shift) - 1;
}

void LatencyHistogram::record_us(std::uint64_t us) noexcept {
    buckets_[static_cast<std::size_t>(bucket_of(us))].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(us, std::memory_order_relaxed);
    std::uint64_t cur = min_.load(std::memory_order_relaxed);
    while (us < cur && !min_.compare_exchange_weak(cur, us, std::memory_order_relaxed)) {
    }
    cur = max_.load(std::memory_order_relaxed);
    while (us > cur && !max_.compare_exchange_weak(cur, us, std::memory_order_relaxed)) {
    }
}

LatencySnapshot LatencyHistogram::snapshot() const noexcept {
    LatencySnapshot s;
    std::array<std::uint64_t, kBuckets> b{};
    std::uint64_t total = 0;
    for (int i = 0; i < kBuckets; ++i) {
        b[static_cast<std::size_t>(i)] = buckets_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        total += b[static_cast<std::size_t>(i)];
    }
    s.count = total;
    if (total == 0) return s;
    s.mean_us = static_cast<double>(sum_.load(std::memory_order_relaxed)) /
                static_cast<double>(count_.load(std::memory_order_relaxed));
    s.min_us = static_cast<double>(min_.load(std::memory_order_relaxed));
    s.max_us = static_cast<double>(max_.load(std::memory_order_relaxed));
    auto pct = [&](double p) {
        const auto rank = static_cast<std::uint64_t>(p * static_cast<double>(total - 1)) + 1;
        std::uint64_t acc = 0;
        for (int i = 0; i < kBuckets; ++i) {
            acc += b[static_cast<std::size_t>(i)];
            if (acc >= rank) {
                double v = static_cast<double>(bucket_upper(i));
                return v > s.max_us ? s.max_us : v;
            }
        }
        return s.max_us;
    };
    s.p50_us = pct(0.50);
    s.p95_us = pct(0.95);
    s.p99_us = pct(0.99);
    return s;
}

void LatencyHistogram::reset() noexcept {
    for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
    count_.store(0);
    sum_.store(0);
    min_.store(UINT64_MAX);
    max_.store(0);
}

double ThroughputMeter::sample(std::uint64_t now_ns) {
    std::lock_guard lk(m_);
    const std::uint64_t total = total_.load(std::memory_order_relaxed);
    if (last_ns_ != 0 && now_ns > last_ns_) {
        const double inst = static_cast<double>(total - last_total_) * 1e9 / static_cast<double>(now_ns - last_ns_);
        const double prev = rate_.load(std::memory_order_relaxed);
        // EWMA (alpha 0.5) so the UI reacts within ~2 samples but is not jumpy.
        rate_.store(prev == 0.0 ? inst : 0.5 * prev + 0.5 * inst, std::memory_order_relaxed);
    }
    last_total_ = total;
    last_ns_ = now_ns;
    return rate_.load(std::memory_order_relaxed);
}

double LinkMetrics::packet_loss() const noexcept {
    const auto sent = pings_sent.get();
    const auto recv = pongs_received.get();
    // the most recent ping may legitimately be in flight
    if (sent <= 1 || recv >= sent - 1) return 0.0;
    return static_cast<double>(sent - 1 - recv) / static_cast<double>(sent - 1);
}

nlohmann::json to_json(const LatencySnapshot& s) {
    return {{"count", s.count}, {"mean_us", s.mean_us}, {"min_us", s.min_us}, {"max_us", s.max_us},
            {"p50_us", s.p50_us}, {"p95_us", s.p95_us}, {"p99_us", s.p99_us}};
}

nlohmann::json to_json(const LinkMetrics& m) {
    return {{"tx_bytes", m.tx.total()},
            {"rx_bytes", m.rx.total()},
            {"tx_bps", m.tx.rate() * 8},
            {"rx_bps", m.rx.rate() * 8},
            {"frames_tx", m.frames_tx.get()},
            {"frames_rx", m.frames_rx.get()},
            {"rtt", to_json(m.rtt.snapshot())},
            {"last_rtt_us", m.last_rtt_us.load()},
            {"packet_loss", m.packet_loss()},
            {"reconnects", m.reconnects.get()},
            {"protocol_errors", m.protocol_errors.get()}};
}

nlohmann::json to_json(const UsbMetrics& m) {
    return {{"transfers", m.transfers.get()},
            {"errors", m.errors.get()},
            {"timeouts", m.timeouts.get()},
            {"stalls", m.stalls.get()},
            {"cancels", m.cancels.get()},
            {"resets", m.resets.get()},
            {"bytes_in", m.bytes_in.total()},
            {"bytes_out", m.bytes_out.total()},
            {"in_bps", m.bytes_in.rate() * 8},
            {"out_bps", m.bytes_out.rate() * 8},
            {"latency", to_json(m.latency.snapshot())}};
}

}  // namespace pf
