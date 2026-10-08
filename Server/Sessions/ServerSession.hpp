// A redirected-device session on the server (see ARCHITECTURE.md §2.6).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <asio.hpp>
#include <asio/thread_pool.hpp>
#include <nlohmann/json_fwd.hpp>

#include "Diagnostics/Metrics.hpp"
#include "Models/DeviceInfo.hpp"
#include "Models/SessionState.hpp"
#include "Network/FrameConnection.hpp"
#include "USB/UsbBackend.hpp"

namespace pf::server {

// Executor that serializes blocking lifecycle work (open/close/reset/configure) for
// one physical port, so e.g. a close always finishes before the next open.
using DeviceExecutor = asio::strand<asio::thread_pool::executor_type>;

class ServerSession : public std::enable_shared_from_this<ServerSession> {
public:
    struct Params {
        std::string id;
        std::string resume_token;
        std::string owner_client_id;
        std::string owner_name;
        std::uint32_t handle = 0;
        AccessMode access = AccessMode::Exclusive;
        SessionPriority priority = SessionPriority::High;
        bool mobile_repair = true;
    };

    ServerSession(Params p, asio::io_context& io);
    ~ServerSession();

    // ---- identity ----
    [[nodiscard]] const std::string& id() const noexcept { return p_.id; }
    [[nodiscard]] const std::string& resume_token() const noexcept { return p_.resume_token; }
    [[nodiscard]] const std::string& owner_client_id() const noexcept { return p_.owner_client_id; }
    [[nodiscard]] const std::string& owner_name() const noexcept { return p_.owner_name; }
    [[nodiscard]] std::uint32_t handle() const noexcept { return p_.handle; }
    [[nodiscard]] AccessMode access() const noexcept { return p_.access; }
    [[nodiscard]] SessionPriority priority() const noexcept { return p_.priority; }
    SessionStateMachine& state_machine() noexcept { return sm_; }
    [[nodiscard]] SessionState state() const { return sm_.state(); }
    UsbMetrics& metrics() noexcept { return metrics_; }

    // ---- device binding ----
    void bind_device(std::shared_ptr<IUsbDevice> dev, const DeviceInfo& info, std::optional<DeviceExecutor> exec);
    // Detaches the device without closing it (caller closes it on the device executor).
    std::shared_ptr<IUsbDevice> release_device();
    // Device disappeared: cancels transfers, releases the handle. Returns the identity
    // used later for re-enumeration matching.
    void device_lost();
    [[nodiscard]] bool has_device() const;
    [[nodiscard]] DeviceInfo device_info() const;
    [[nodiscard]] DescriptorSet descriptors() const;
    [[nodiscard]] UsbSpeed speed() const;
    [[nodiscard]] std::int64_t device_lost_at_ms() const { return lost_at_ms_.load(); }

    // ---- connection binding ----
    void set_connection(std::shared_ptr<net::FrameConnection> c);
    [[nodiscard]] std::shared_ptr<net::FrameConnection> connection() const;
    // Transport lost: cancel in-flight work (results are dropped).
    void connection_lost();

    // ---- data plane (called on the connection strand) ----
    void submit(proto::UsbSubmitMsg& msg, proto::InFrame&& frame);
    void cancel(std::uint64_t seq);
    void reset(const proto::UsbResetMsg& m, std::function<void(Status)> on_done);
    void configure(const proto::UsbConfigMsg& m);

    // ---- lifecycle ----
    // Arms the grace timer; `on_expire` runs on the I/O context if the session is still
    // Suspended/Reconnecting when it fires.
    void arm_grace_timer(std::uint32_t ms, std::function<void()> on_expire);
    void disarm_grace_timer();
    // Releases the device (blocking work is offloaded). Idempotent.
    void shutdown();

    [[nodiscard]] std::size_t inflight() const;
    [[nodiscard]] nlohmann::json to_json() const;

private:
    void complete(TransferResult&& r, std::uint32_t generation);
    void send_op_result(std::uint64_t seq, UsbStatus st);
    std::shared_ptr<IUsbDevice> device() const;
    std::optional<DeviceExecutor> executor() const;

    Params p_;
    asio::io_context& io_;
    std::optional<DeviceExecutor> exec_;  // guarded by m_
    SessionStateMachine sm_;
    UsbMetrics metrics_;
    asio::steady_timer grace_timer_;
    std::int64_t created_ms_;

    mutable std::mutex m_;
    std::shared_ptr<IUsbDevice> usb_;
    DeviceInfo info_;
    DescriptorSet descriptors_;
    UsbSpeed speed_ = UsbSpeed::Unknown;
    std::uint32_t generation_ = 0;  // bumped on every bind/lost; stale completions are dropped
    std::weak_ptr<net::FrameConnection> conn_;
    struct InFlight {
        std::uint64_t start_ns;
        std::uint32_t length;
        bool in;
    };
    std::unordered_map<std::uint64_t, InFlight> inflight_;
    std::atomic<std::int64_t> lost_at_ms_{0};
};

}  // namespace pf::server
