#include "Network/FrameConnection.hpp"

#include "Common/Platform.hpp"
#include "Logging/Logger.hpp"

namespace pf::net {

using namespace proto;

std::shared_ptr<FrameConnection> FrameConnection::create(TransportPtr t, ConnectionOptions opts,
                                                         std::shared_ptr<LinkMetrics> metrics) {
    if (!metrics) metrics = std::make_shared<LinkMetrics>();
    return std::shared_ptr<FrameConnection>(new FrameConnection(std::move(t), std::move(opts), std::move(metrics)));
}

FrameConnection::FrameConnection(TransportPtr t, ConnectionOptions opts, std::shared_ptr<LinkMetrics> metrics)
    : transport_(std::move(t)),
      opts_(std::move(opts)),
      metrics_(std::move(metrics)),
      decoder_(opts_.tuning.max_frame_size, opts_.tuning.read_buffer_size),
      keepalive_timer_(transport_->strand()) {
    remote_ = transport_->remote_address();
    fingerprint_ = transport_->peer_fingerprint();
    secure_ = transport_->is_secure();
}

FrameConnection::~FrameConnection() = default;

void FrameConnection::start(ConnectionHandlers h) {
    asio::post(strand(), [self = shared_from_this(), h = std::move(h)]() mutable {
        self->handlers_ = std::move(h);
        bool kick = false;
        {
            std::lock_guard lk(self->send_m_);
            if (self->started_) return;
            self->started_ = true;
            // frames queued before start
            if (!self->writing_ && self->queued_bytes_.load() > 0) {
                self->writing_ = true;
                kick = true;
            }
        }
        if (self->closed_) return;
        self->do_read();
        if (self->opts_.send_pings) self->schedule_keepalive();
        if (kick) self->start_write();
    });
}

// ---------------------------------------------------------------- reading

void FrameConnection::do_read() {
    if (closed_ || read_pending_ || read_paused_) return;
    read_pending_ = true;
    auto buf = decoder_.prepare();
    transport_->async_read_some(buf, [self = shared_from_this()](std::error_code ec, std::size_t n) {
        self->on_read(ec, n);
    });
}

void FrameConnection::on_read(std::error_code ec, std::size_t n) {
    read_pending_ = false;
    if (closed_) return;
    if (ec) {
        close_on_strand(error_from_asio(ec, "read"));
        return;
    }
    metrics_->rx.add(n);
    received_since_tick_ = true;
    auto st = decoder_.commit(n, [this](InFrame&& f) { handle_frame(std::move(f)); });
    if (!st) {
        metrics_->protocol_errors.add();
        send(encode_error(st.error().code, st.error().message), true);
        close_after_flush(st.error());
        return;
    }
    do_read();
}

void FrameConnection::handle_frame(InFrame&& f) {
    if (closed_) return;
    metrics_->frames_rx.add();
    outstanding_pings_ = 0;  // any traffic proves liveness
    switch (f.header.type) {
        case MsgType::Ping: {
            PingMsg p;
            if (decode_ping(f.bytes(), p)) send(encode_ping(MsgType::Pong, p), true);
            return;
        }
        case MsgType::Pong: {
            PingMsg p;
            if (decode_ping(f.bytes(), p)) {
                const std::uint64_t now = monotonic_ns();
                if (now >= p.sent_ns) {
                    const std::uint64_t us = (now - p.sent_ns) / 1000;
                    metrics_->rtt.record_us(us);
                    metrics_->last_rtt_us.store(us);
                }
                metrics_->pongs_received.add();
            }
            return;
        }
        default:
            break;
    }
    if (handlers_.on_frame) handlers_.on_frame(std::move(f));
}

void FrameConnection::pause_reading() { read_paused_ = true; }

void FrameConnection::resume_reading() {
    if (!read_paused_.exchange(false)) return;
    asio::post(strand(), [self = shared_from_this()] { self->do_read(); });
}

// ---------------------------------------------------------------- writing

void FrameConnection::send(OutFrame&& f, bool urgent) {
    if (closed_) return;
    const std::size_t sz = f.buf.size();
    bool notify_congested = false;
    bool kick = false;
    {
        std::lock_guard lk(send_m_);
        if (urgent) {
            urgent_.push_back(std::move(f));
        } else {
            const std::size_t prio = f.priority > 3 ? 3 : f.priority;
            data_[prio].push_back(std::move(f));
        }
        const std::size_t q = queued_bytes_.fetch_add(sz, std::memory_order_relaxed) + sz;
        if (q > opts_.tuning.send_high_watermark && !congested_.load(std::memory_order_relaxed)) {
            congested_.store(true, std::memory_order_relaxed);
            notify_congested = true;
        }
        if (!writing_ && started_) {
            writing_ = true;
            kick = true;
        }
    }
    if (kick) asio::post(strand(), [self = shared_from_this()] { self->start_write(); });
    if (notify_congested) {
        asio::post(strand(), [self = shared_from_this()] {
            if (self->handlers_.on_backpressure) self->handlers_.on_backpressure(true);
        });
    }
}

void FrameConnection::start_write() {
    if (closed_) return;
    in_flight_.clear();
    write_bufs_.clear();
    std::size_t batch = 0;
    {
        std::lock_guard lk(send_m_);
        auto take = [&](std::deque<OutFrame>& q) {
            batch += q.front().buf.size();
            in_flight_.push_back(std::move(q.front()));
            q.pop_front();
        };
        while (!urgent_.empty() && batch < opts_.tuning.max_write_batch && in_flight_.size() < 64) take(urgent_);
        while (batch < opts_.tuning.max_write_batch && in_flight_.size() < 64) {
            // Strict priority, except every 8th pick goes to the highest-priority
            // non-empty queue below the top one so low priority sessions never starve.
            int top = -1;
            for (int p = 3; p >= 0; --p) {
                if (!data_[static_cast<std::size_t>(p)].empty()) {
                    top = p;
                    break;
                }
            }
            if (top < 0) break;
            int pick = top;
            if ((++fairness_counter_ & 7) == 0) {
                for (int p = top - 1; p >= 0; --p) {
                    if (!data_[static_cast<std::size_t>(p)].empty()) {
                        pick = p;
                        break;
                    }
                }
            }
            take(data_[static_cast<std::size_t>(pick)]);
        }
        if (in_flight_.empty()) {
            writing_ = false;
            if (flush_then_close_) {
                Error r = flush_close_reason_;
                asio::post(strand(), [self = shared_from_this(), r] { self->close_on_strand(r); });
            }
            return;
        }
    }
    for (auto& f : in_flight_) write_bufs_.emplace_back(f.buf.data(), f.buf.size());
    transport_->async_write(write_bufs_, [self = shared_from_this()](std::error_code ec, std::size_t n) {
        self->on_written(ec, n);
    });
}

void FrameConnection::on_written(std::error_code ec, std::size_t n) {
    if (ec) {
        in_flight_.clear();
        close_on_strand(error_from_asio(ec, "write"));
        return;
    }
    metrics_->tx.add(n);
    metrics_->frames_tx.add(in_flight_.size());
    in_flight_.clear();
    const std::size_t q = queued_bytes_.fetch_sub(n, std::memory_order_relaxed) - n;
    if (congested_.load(std::memory_order_relaxed) && q < opts_.tuning.send_low_watermark) {
        congested_.store(false, std::memory_order_relaxed);
        if (handlers_.on_backpressure) handlers_.on_backpressure(false);
    }
    start_write();
}

// ---------------------------------------------------------------- keepalive

void FrameConnection::schedule_keepalive() {
    keepalive_timer_.expires_after(std::chrono::milliseconds(opts_.keepalive.interval_ms));
    keepalive_timer_.async_wait([self = shared_from_this()](std::error_code ec) {
        if (!ec) self->on_keepalive_tick();
    });
}

void FrameConnection::on_keepalive_tick() {
    if (closed_) return;
    if (!received_since_tick_ && outstanding_pings_ >= opts_.keepalive.miss_limit) {
        close_on_strand(Error{ErrorCode::ConnectionLost, "keepalive timeout (peer not responding)"});
        return;
    }
    received_since_tick_ = false;
    ++outstanding_pings_;
    metrics_->pings_sent.add();
    send(encode_ping(MsgType::Ping, PingMsg{++ping_seq_, monotonic_ns()}), true);
    schedule_keepalive();
}

// ---------------------------------------------------------------- closing

void FrameConnection::close(Error reason) {
    asio::post(strand(), [self = shared_from_this(), reason = std::move(reason)] { self->close_on_strand(reason); });
}

void FrameConnection::close_after_flush(Error reason) {
    bool close_now = false;
    {
        std::lock_guard lk(send_m_);
        flush_then_close_ = true;
        flush_close_reason_ = reason;
        close_now = !writing_;
    }
    if (close_now) close(std::move(reason));
}

void FrameConnection::close_on_strand(Error reason) {
    if (closed_.exchange(true)) return;
    std::error_code ignore;
    keepalive_timer_.cancel();
    transport_->close();
    {
        std::lock_guard lk(send_m_);
        urgent_.clear();
        for (auto& q : data_) q.clear();
        queued_bytes_ = 0;
    }
    auto handler = std::move(handlers_.on_closed);
    handlers_ = {};
    if (handler) handler(reason);
}

}  // namespace pf::net
