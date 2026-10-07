// A PFUP frame connection over a Transport.
//
//  * read loop with zero-copy large-payload reads (FrameDecoder)
//  * send queue: urgent (control/link) frames first, then data frames by session
//    priority with a starvation guard; several frames are gathered into one write
//  * byte-based backpressure with high/low watermarks
//  * PING/PONG keepalive with RTT measurement and dead-peer detection
//
// send() and close() are thread-safe; all callbacks run on the connection strand.
#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "Common/Config.hpp"
#include "Diagnostics/Metrics.hpp"
#include "Network/Transport.hpp"
#include "Protocol/Protocol.hpp"

namespace pf::net {

struct ConnectionOptions {
    NetworkTuning tuning;
    KeepaliveConfig keepalive;
    bool send_pings = true;
};

struct ConnectionHandlers {
    std::function<void(proto::InFrame&&)> on_frame;
    std::function<void(const Error&)> on_closed;        // exactly once
    std::function<void(bool congested)> on_backpressure; // true above high, false below low watermark
};

class FrameConnection : public std::enable_shared_from_this<FrameConnection> {
public:
    static std::shared_ptr<FrameConnection> create(TransportPtr t, ConnectionOptions opts,
                                                   std::shared_ptr<LinkMetrics> metrics = nullptr);
    ~FrameConnection();

    void start(ConnectionHandlers h);

    // urgent: control-plane/link frames that must overtake queued bulk data.
    void send(proto::OutFrame&& f, bool urgent = false);
    // Closes immediately (pending output dropped).
    void close(Error reason);
    // Sends everything queued, then closes.
    void close_after_flush(Error reason);

    // Receive-side flow control (e.g. too many USB transfers in flight).
    void pause_reading();
    void resume_reading();

    [[nodiscard]] bool congested() const noexcept { return congested_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t queued_bytes() const noexcept { return queued_bytes_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool closed() const noexcept { return closed_.load(); }
    [[nodiscard]] std::string remote_address() const { return remote_; }
    [[nodiscard]] bool is_secure() const noexcept { return secure_; }
    [[nodiscard]] std::string peer_fingerprint() const { return fingerprint_; }
    [[nodiscard]] const std::shared_ptr<LinkMetrics>& metrics() const noexcept { return metrics_; }
    [[nodiscard]] std::uint64_t last_rtt_us() const noexcept { return metrics_->last_rtt_us.load(); }
    Strand& strand() noexcept { return transport_->strand(); }

private:
    FrameConnection(TransportPtr t, ConnectionOptions opts, std::shared_ptr<LinkMetrics> metrics);
    void do_read();
    void on_read(std::error_code ec, std::size_t n);
    void handle_frame(proto::InFrame&& f);
    void start_write();
    void on_written(std::error_code ec, std::size_t n);
    void schedule_keepalive();
    void on_keepalive_tick();
    void close_on_strand(Error reason);

    TransportPtr transport_;
    ConnectionOptions opts_;
    std::shared_ptr<LinkMetrics> metrics_;
    ConnectionHandlers handlers_;
    proto::FrameDecoder decoder_;
    asio::steady_timer keepalive_timer_;
    std::string remote_;
    std::string fingerprint_;
    bool secure_ = false;

    // send state (guarded by send_m_)
    std::mutex send_m_;
    std::deque<proto::OutFrame> urgent_;
    std::array<std::deque<proto::OutFrame>, 4> data_;  // index = priority (0 low .. 3 critical)
    unsigned fairness_counter_ = 0;
    bool writing_ = false;
    bool flush_then_close_ = false;
    Error flush_close_reason_;
    std::vector<proto::OutFrame> in_flight_;     // frames of the current write
    std::vector<asio::const_buffer> write_bufs_;

    std::atomic<std::size_t> queued_bytes_{0};
    std::atomic<bool> congested_{false};
    std::atomic<bool> closed_{false};
    std::atomic<bool> read_paused_{false};
    bool read_pending_ = false;                  // strand only
    bool started_ = false;                       // guarded by send_m_

    // keepalive (strand only)
    std::uint64_t ping_seq_ = 0;
    std::uint32_t outstanding_pings_ = 0;
    bool received_since_tick_ = false;
};

}  // namespace pf::net
