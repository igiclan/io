// Client side of a redirected-device session: bridges the virtual host controller
// (URB records) and the PFUP data plane.
#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

#include "Diagnostics/Metrics.hpp"
#include "Models/DeviceInfo.hpp"
#include "Models/SessionState.hpp"
#include "Network/FrameConnection.hpp"
#include "VirtualUSB/VirtualHci.hpp"

namespace pf::client {

struct SessionParams {
    std::string session_id;
    std::string resume_token;
    std::uint32_t handle = 0;  // server handle
    std::string server;        // server name
    DeviceInfo device;
    DescriptorSet descriptors;
    UsbSpeed speed = UsbSpeed::High;
    AccessMode access = AccessMode::Exclusive;
    SessionPriority priority = SessionPriority::High;
    std::size_t max_held_bytes = 64u << 20;  // URBs buffered while the link is down
};

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
    ClientSession(SessionParams p, IVirtualHci& vhci, std::uint64_t cookie);
    ~ClientSession();

    [[nodiscard]] const std::string& id() const noexcept { return p_.session_id; }
    [[nodiscard]] const std::string& server() const noexcept { return p_.server; }
    [[nodiscard]] std::uint32_t handle() const noexcept { return p_.handle; }
    [[nodiscard]] std::string resume_token() const;
    [[nodiscard]] std::uint64_t cookie() const noexcept { return cookie_; }
    [[nodiscard]] DeviceInfo device() const;
    [[nodiscard]] std::uint32_t vhci_handle() const;
    SessionStateMachine& state_machine() noexcept { return sm_; }
    [[nodiscard]] SessionState state() const { return sm_.state(); }
    UsbMetrics& metrics() noexcept { return metrics_; }

    // Plugs the virtual device (Windows enumerates it).
    Status plug();
    void unplug();

    // ---- VHCI side (fetch threads) ----
    void on_vhci_request(const VhciRequest& r);

    // ---- network side (link strand) ----
    void on_complete(const proto::UsbCompleteMsg& m);
    void on_op_result(const proto::UsbOpResultMsg& m);
    void link_up(std::shared_ptr<net::FrameConnection> conn);  // after attach or resume
    void link_down();                                         // transport lost
    void device_removed();                                     // server: device left (mode switch)
    Status device_reenumerated(const DescriptorSet& d, UsbSpeed speed, const DeviceInfo& info);
    void close(const std::string& reason);

    [[nodiscard]] std::size_t inflight() const;
    [[nodiscard]] std::size_t held() const;
    [[nodiscard]] nlohmann::json to_json() const;

private:
    struct Pending {
        std::uint64_t request_id;
        std::uint32_t vhci_handle;
        std::uint64_t start_ns;
        std::uint32_t length;
        bool in;
        bool op;
    };
    void send_or_hold(proto::OutFrame&& f, std::uint64_t seq, Pending p);
    void fail_all_inflight(UsbStatus st);
    void complete_vhci(const Pending& p, UsbStatus st, std::uint32_t actual = 0, ByteSpan data = {},
                       std::vector<proto::IsoResult> iso = {}, std::uint32_t iso_errors = 0);

    SessionParams p_;
    IVirtualHci& vhci_;
    const std::uint64_t cookie_;
    SessionStateMachine sm_;
    UsbMetrics metrics_;

    mutable std::mutex m_;
    std::weak_ptr<net::FrameConnection> link_;
    std::uint32_t vhci_handle_ = 0;
    std::uint64_t next_seq_ = 1;
    std::unordered_map<std::uint64_t, Pending> inflight_;       // seq -> pending
    std::unordered_map<std::uint64_t, std::uint64_t> by_request_;  // vhci request id -> seq
    struct Held {
        proto::OutFrame frame;
        std::uint64_t seq;
        Pending p;
    };
    std::deque<Held> held_;
    std::size_t held_bytes_ = 0;
};

}  // namespace pf::client
