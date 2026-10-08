#include "Sessions/ServerSession.hpp"

#include <cstring>
#include <nlohmann/json.hpp>

#include "Common/Platform.hpp"
#include "Logging/Logger.hpp"

namespace pf::server {

using namespace proto;

ServerSession::ServerSession(Params p, asio::io_context& io)
    : p_(std::move(p)), io_(io), grace_timer_(io), created_ms_(unix_time_ms()) {}

namespace {
// Runs blocking device work on the session's device executor (or inline-posts to the
// I/O context when no executor is bound, which only happens for read-only sessions).
template <typename F>
void run_blocking(const std::optional<DeviceExecutor>& exec, asio::io_context& io, F&& f) {
    if (exec) {
        asio::post(*exec, std::forward<F>(f));
    } else {
        asio::post(io, std::forward<F>(f));
    }
}
}  // namespace

ServerSession::~ServerSession() = default;

// ------------------------------------------------------------ device binding

void ServerSession::bind_device(std::shared_ptr<IUsbDevice> dev, const DeviceInfo& info,
                                std::optional<DeviceExecutor> exec) {
    std::lock_guard lk(m_);
    usb_ = std::move(dev);
    exec_ = std::move(exec);
    info_ = info;
    descriptors_ = usb_ ? usb_->descriptors() : DescriptorSet{};
    speed_ = usb_ ? usb_->speed() : info.identity.speed;
    ++generation_;
    lost_at_ms_ = 0;
}

std::shared_ptr<IUsbDevice> ServerSession::release_device() {
    std::lock_guard lk(m_);
    ++generation_;
    inflight_.clear();
    return std::move(usb_);
}

void ServerSession::device_lost() {
    std::shared_ptr<IUsbDevice> dev;
    std::optional<DeviceExecutor> exec;
    std::unordered_map<std::uint64_t, InFlight> victims;
    std::shared_ptr<net::FrameConnection> conn;
    {
        std::lock_guard lk(m_);
        dev = std::move(usb_);
        exec = exec_;
        ++generation_;
        victims.swap(inflight_);
        conn = conn_.lock();
    }
    lost_at_ms_ = unix_time_ms();
    // Fail everything the client is waiting for: the device is gone.
    if (conn) {
        for (const auto& [seq, f] : victims) {
            UsbCompleteMsg c;
            c.seq = seq;
            c.device = p_.handle;
            c.status = UsbStatus::DeviceGone;
            auto frame = encode_complete(c);
            frame.priority = static_cast<std::uint8_t>(p_.priority);
            conn->send(std::move(frame));
        }
    }
    if (dev) run_blocking(exec, io_, [dev] { dev->close(); });
}

bool ServerSession::has_device() const {
    std::lock_guard lk(m_);
    return usb_ != nullptr;
}

DeviceInfo ServerSession::device_info() const {
    std::lock_guard lk(m_);
    return info_;
}

DescriptorSet ServerSession::descriptors() const {
    std::lock_guard lk(m_);
    return descriptors_;
}

UsbSpeed ServerSession::speed() const {
    std::lock_guard lk(m_);
    return speed_;
}

std::shared_ptr<IUsbDevice> ServerSession::device() const {
    std::lock_guard lk(m_);
    return usb_;
}

// ------------------------------------------------------------ connection

void ServerSession::set_connection(std::shared_ptr<net::FrameConnection> c) {
    std::lock_guard lk(m_);
    conn_ = std::move(c);
}

std::shared_ptr<net::FrameConnection> ServerSession::connection() const {
    std::lock_guard lk(m_);
    return conn_.lock();
}

void ServerSession::connection_lost() {
    std::shared_ptr<IUsbDevice> dev;
    {
        std::lock_guard lk(m_);
        conn_.reset();
        dev = usb_;
        // Results of in-flight transfers can no longer be delivered; the client fails
        // them locally. Cancel so the device is quiescent when the client resumes.
        inflight_.clear();
        ++generation_;
    }
    if (dev) dev->cancel_all();
}

// ------------------------------------------------------------ data plane

void ServerSession::submit(UsbSubmitMsg& msg, InFrame&& frame) {
    auto conn = connection();
    auto reply_error = [&](UsbStatus st) {
        if (!conn) return;
        UsbCompleteMsg c;
        c.seq = msg.seq;
        c.device = p_.handle;
        c.status = st;
        auto f = encode_complete(c);
        f.priority = static_cast<std::uint8_t>(p_.priority);
        conn->send(std::move(f));
    };
    if (p_.access == AccessMode::SharedReadOnly) return reply_error(UsbStatus::NotAuthorized);
    if (msg.length > (16u << 20)) return reply_error(UsbStatus::InvalidRequest);

    // Standard requests that must go through the host API instead of EP0.
    if (msg.type == TransferType::Control) {
        const SpecialControl sc = classify_control(msg.setup);
        if (sc != SpecialControl::None) {
            UsbConfigMsg cm;
            cm.seq = msg.seq;
            cm.device = p_.handle;
            cm.op = sc == SpecialControl::SetConfiguration ? ConfigOp::SetConfiguration
                    : sc == SpecialControl::SetInterface   ? ConfigOp::SetInterface
                                                           : ConfigOp::ClearHalt;
            cm.value = sc == SpecialControl::SetConfiguration ? msg.setup[2] : msg.setup[4];  // wValue / wIndex
            cm.alt = msg.setup[2];
            auto dev = device();
            if (!dev) return reply_error(UsbStatus::DeviceGone);
            run_blocking(executor(), io_, [self = shared_from_this(), dev, cm, seq = msg.seq] {
                Status st = cm.op == ConfigOp::SetConfiguration ? dev->set_configuration(cm.value)
                            : cm.op == ConfigOp::SetInterface   ? dev->set_interface(cm.value, cm.alt)
                                                                : dev->clear_halt(cm.value);
                UsbCompleteMsg c;
                c.seq = seq;
                c.device = self->p_.handle;
                c.status = st.ok() ? UsbStatus::Success : UsbStatus::Stall;
                if (auto cn = self->connection()) {
                    auto f = encode_complete(c);
                    f.priority = static_cast<std::uint8_t>(self->p_.priority);
                    cn->send(std::move(f));
                }
            });
            return;
        }
    }

    TransferRequest req;
    req.seq = msg.seq;
    req.endpoint = msg.endpoint;
    req.type = msg.type;
    req.flags = msg.flags;
    req.timeout_ms = msg.timeout_ms;
    req.length = msg.length;
    req.setup = msg.setup;
    req.iso = msg.iso;
    if (msg.is_in()) {
        // Reserve the completion frame prefix (which also covers the 8 setup bytes
        // libusb expects in front of control data).
        const std::size_t prefix = complete_prefix_size(msg.iso.size());
        req.buffer = BufferPool::global().acquire(prefix + msg.length);
        req.data_offset = prefix;
    } else if (msg.type == TransferType::Control) {
        req.buffer = BufferPool::global().acquire(8 + msg.length);
        req.data_offset = 8;
        if (msg.length) std::memcpy(req.buffer.data() + 8, msg.out_data.data(), msg.length);
    } else {
        // Zero copy: transfer straight out of the received frame payload.
        req.data_offset = static_cast<std::size_t>(msg.out_data.data() - frame.payload.data());
        req.buffer = std::move(frame.payload);
    }

    std::shared_ptr<IUsbDevice> dev;
    std::uint32_t gen;
    {
        std::lock_guard lk(m_);
        dev = usb_;
        gen = generation_;
        if (dev) inflight_[msg.seq] = InFlight{monotonic_ns(), msg.length, msg.is_in()};
    }
    if (!dev) return reply_error(UsbStatus::DeviceGone);
    metrics_.transfers.add();
    dev->submit(std::move(req), [w = weak_from_this(), gen](TransferResult&& r) {
        if (auto self = w.lock()) self->complete(std::move(r), gen);
    });
}

void ServerSession::complete(TransferResult&& r, std::uint32_t generation) {
    InFlight f{};
    std::shared_ptr<net::FrameConnection> conn;
    {
        std::lock_guard lk(m_);
        if (generation != generation_) return;  // stale (device lost / reconnected)
        auto it = inflight_.find(r.seq);
        if (it == inflight_.end()) return;
        f = it->second;
        inflight_.erase(it);
        conn = conn_.lock();
    }
    metrics_.latency.record_ns(monotonic_ns() - f.start_ns);
    switch (r.status) {
        case UsbStatus::Success: break;
        case UsbStatus::Timeout: metrics_.timeouts.add(); break;
        case UsbStatus::Stall: metrics_.stalls.add(); break;
        case UsbStatus::Cancelled: metrics_.cancels.add(); break;
        default: metrics_.errors.add(); break;
    }
    if (f.in) {
        metrics_.bytes_in.add(r.actual_length);
    } else {
        metrics_.bytes_out.add(r.actual_length);
    }
    if (!conn) return;
    UsbCompleteMsg c;
    c.seq = r.seq;
    c.device = p_.handle;
    c.status = r.status;
    c.actual_length = r.actual_length;
    c.iso_error_count = r.iso_error_count;
    c.iso = std::move(r.iso);
    OutFrame frame;
    if (f.in && r.buffer && r.data_offset == complete_prefix_size(c.iso.size())) {
        std::size_t data_len = r.actual_length;
        if (!c.iso.empty()) {
            // iso IN: buffer from offset 0 to the end of the last packet (PROTOCOL.md §6.2)
            data_len = r.buffer.size() - r.data_offset;
        }
        frame = finalize_complete_inplace(std::move(r.buffer), c, data_len);
    } else {
        frame = encode_complete(c);
    }
    frame.priority = static_cast<std::uint8_t>(p_.priority);
    conn->send(std::move(frame));
}

void ServerSession::cancel(std::uint64_t seq) {
    if (auto dev = device()) dev->cancel(seq);
}

void ServerSession::send_op_result(std::uint64_t seq, UsbStatus st) {
    if (auto conn = connection()) {
        auto f = encode_op_result(UsbOpResultMsg{seq, p_.handle, st});
        conn->send(std::move(f), true);
    }
}

void ServerSession::reset(const UsbResetMsg& m, std::function<void(Status)> on_done) {
    auto dev = device();
    if (!dev || p_.access == AccessMode::SharedReadOnly) {
        send_op_result(m.seq, dev ? UsbStatus::NotAuthorized : UsbStatus::DeviceGone);
        return;
    }
    metrics_.resets.add();
    run_blocking(executor(), io_, [self = shared_from_this(), dev, m, on_done = std::move(on_done)] {
        Status st = dev->reset_port();
        self->send_op_result(m.seq, st.ok() ? UsbStatus::Success
                                            : st.error().code == ErrorCode::DeviceGone ? UsbStatus::DeviceGone
                                                                                       : UsbStatus::IoError);
        LogRecord(st.ok() ? LogLevel::Info : LogLevel::Warning, "USB")
            .session(self->id())
            .op("reset")
            .result(st.ok() ? "ok" : st.error().describe());
        if (on_done) on_done(st);
    });
}

void ServerSession::configure(const UsbConfigMsg& m) {
    auto dev = device();
    if (!dev || p_.access == AccessMode::SharedReadOnly) {
        send_op_result(m.seq, dev ? UsbStatus::NotAuthorized : UsbStatus::DeviceGone);
        return;
    }
    run_blocking(executor(), io_, [self = shared_from_this(), dev, m] {
        Status st = m.op == ConfigOp::SetConfiguration ? dev->set_configuration(m.value)
                    : m.op == ConfigOp::SetInterface   ? dev->set_interface(m.value, m.alt)
                                                       : dev->clear_halt(m.value);
        self->send_op_result(m.seq, st.ok() ? UsbStatus::Success : UsbStatus::Stall);
    });
}

// ------------------------------------------------------------ lifecycle

void ServerSession::arm_grace_timer(std::uint32_t ms, std::function<void()> on_expire) {
    grace_timer_.expires_after(std::chrono::milliseconds(ms));
    grace_timer_.async_wait([w = weak_from_this(), on_expire = std::move(on_expire)](std::error_code ec) {
        if (ec) return;
        auto self = w.lock();
        if (!self) return;
        const auto st = self->state();
        if (st == SessionState::Suspended || st == SessionState::Reconnecting) on_expire();
    });
}

void ServerSession::disarm_grace_timer() { grace_timer_.cancel(); }

void ServerSession::shutdown() {
    grace_timer_.cancel();
    std::shared_ptr<IUsbDevice> dev;
    std::optional<DeviceExecutor> exec;
    {
        std::lock_guard lk(m_);
        dev = std::move(usb_);
        exec = exec_;
        conn_.reset();
        inflight_.clear();
        ++generation_;
    }
    if (dev) run_blocking(exec, io_, [dev] { dev->close(); });
}

std::optional<DeviceExecutor> ServerSession::executor() const {
    std::lock_guard lk(m_);
    return exec_;
}

std::size_t ServerSession::inflight() const {
    std::lock_guard lk(m_);
    return inflight_.size();
}

nlohmann::json ServerSession::to_json() const {
    DeviceInfo info = device_info();
    return {{"session_id", p_.id},
            {"handle", p_.handle},
            {"state", std::string(pf::to_string(state()))},
            {"client_id", p_.owner_client_id},
            {"client_name", p_.owner_name},
            {"device_id", info.id},
            {"device", info.display_name()},
            {"mode", info.mode.label},
            {"access", std::string(pf::to_string(p_.access))},
            {"priority", std::string(pf::to_string(p_.priority))},
            {"mobile_repair", p_.mobile_repair},
            {"created", iso8601_utc(created_ms_)},
            {"inflight", inflight()},
            {"usb", pf::to_json(metrics_)}};
}

}  // namespace pf::server
