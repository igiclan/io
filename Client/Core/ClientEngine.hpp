// ParsFlash USB Client engine (Qt-free): server links, remote device lists, sessions,
// automatic reconnect and session resume. Hosted by the GUI and the CLI.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Common/Config.hpp"
#include "Common/Platform.hpp"
#include "Models/DeviceInfo.hpp"
#include "Network/FrameConnection.hpp"
#include "Sessions/ClientSession.hpp"
#include "VirtualUSB/VirtualHci.hpp"

namespace pf::client {

enum class LinkState : std::uint8_t { Disconnected, Connecting, Authenticating, Connected, Reconnecting, Failed };
std::string_view to_string(LinkState s) noexcept;

struct ServerSnapshot {
    std::string name, host;
    std::uint16_t port = 0;
    bool tls = true;
    LinkState state = LinkState::Disconnected;
    std::string last_error;
    std::string fingerprint;
    std::string server_name;  // as reported by the server
    std::string server_version;
    std::uint64_t rtt_us = 0;
    double tx_bps = 0, rx_bps = 0;
    std::uint64_t reconnects = 0;
    double packet_loss = 0;
    std::size_t devices = 0;
    std::size_t sessions = 0;
};

class ServerLink;

class ClientEngine {
public:
    // token_provider returns the token for a server entry (from the SecretStore).
    using TokenProvider = std::function<std::string(const ServerEntry&)>;
    ClientEngine(ClientConfig cfg, std::unique_ptr<IVirtualHci> vhci, TokenProvider tokens);
    ~ClientEngine();
    ClientEngine(const ClientEngine&) = delete;
    ClientEngine& operator=(const ClientEngine&) = delete;

    Status start();  // opens the VHCI and connects auto_connect servers
    void stop();

    // ---- servers ----
    Status add_server(const ServerEntry& e);
    Status remove_server(const std::string& name);
    Status connect(const std::string& name);
    Status disconnect(const std::string& name);
    [[nodiscard]] std::vector<ServerSnapshot> servers() const;
    [[nodiscard]] std::vector<DeviceInfo> devices(const std::string& server) const;
    [[nodiscard]] ServerEntry server_entry(const std::string& name) const;

    // ---- sessions ----
    using AttachCallback = std::function<void(Result<std::string> session_id)>;
    void attach(const std::string& server, const std::string& device_id, AccessMode access, SessionPriority prio,
                AttachCallback cb);
    Result<std::string> attach_sync(const std::string& server, const std::string& device_id,
                                    AccessMode access = AccessMode::Exclusive,
                                    std::optional<SessionPriority> prio = std::nullopt,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(20));
    Status detach(const std::string& session_id);
    void reset_device(const std::string& session_id);
    [[nodiscard]] std::shared_ptr<ClientSession> session(const std::string& session_id) const;
    [[nodiscard]] std::vector<std::shared_ptr<ClientSession>> sessions() const;

    // ---- diagnostics ----
    [[nodiscard]] nlohmann::json status_json() const;
    [[nodiscard]] nlohmann::json metrics_json() const;
    [[nodiscard]] const ClientConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] VhciInfo vhci_info() const;
    [[nodiscard]] bool vhci_available() const noexcept { return vhci_open_; }
    [[nodiscard]] std::string vhci_error() const;
    asio::io_context& io() noexcept { return rt_->context(); }
    // test hook: forcibly drop a server's TCP connection (simulates network loss)
    void debug_drop_connection(const std::string& server);

    // Events: kind = "server", "devices", "session", "trust" (new fingerprint pinned)
    using EventFn = std::function<void(const std::string& kind, const nlohmann::json& body)>;
    void set_event_handler(EventFn fn);

private:
    friend class ServerLink;
    void emit(const std::string& kind, const nlohmann::json& body);
    void on_vhci_request(const VhciRequest& r);
    void register_session(const std::shared_ptr<ClientSession>& s);
    void unregister_session(const std::shared_ptr<ClientSession>& s);
    void sample_metrics();
    std::shared_ptr<ServerLink> link(const std::string& name) const;

    ClientConfig cfg_;
    std::unique_ptr<IVirtualHci> vhci_;
    TokenProvider tokens_;
    std::unique_ptr<net::IoRuntime> rt_;
    std::unique_ptr<asio::steady_timer> metrics_timer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> vhci_open_{false};
    std::string vhci_error_;
    std::atomic<std::uint64_t> next_cookie_{1};

    mutable std::mutex m_;
    std::map<std::string, std::shared_ptr<ServerLink>> links_;
    std::map<std::string, std::shared_ptr<ClientSession>> sessions_;      // by session id
    std::map<std::uint64_t, std::shared_ptr<ClientSession>> by_cookie_;  // vhci routing
    CpuMeter cpu_;
    double cpu_percent_ = 0;

    std::mutex ev_m_;
    EventFn on_event_;
};

}  // namespace pf::client
