// ParsFlash USB Server engine: device registry, client connections, sessions,
// smart session persistence. Qt-free; hosted by the GUI and the headless server.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <asio/thread_pool.hpp>
#include <nlohmann/json.hpp>

#include "Common/Config.hpp"
#include "Common/Platform.hpp"
#include "Crypto/TokenStore.hpp"
#include "Diagnostics/Metrics.hpp"
#include "Licensing/License.hpp"
#include "Models/DeviceInfo.hpp"
#include "Models/ModeDetector.hpp"
#include "Network/FrameConnection.hpp"
#include "Sessions/ServerSession.hpp"
#include "USB/UsbBackend.hpp"

namespace pf::server {

class ClientConn;

struct ClientSnapshot {
    std::string connection_id;
    std::string client_id;
    std::string name;
    std::string address;
    std::string token_name;
    bool authenticated = false;
    bool secure = false;
    std::int64_t connected_ms = 0;
    std::uint64_t rtt_us = 0;
    double tx_bps = 0, rx_bps = 0;
    std::size_t sessions = 0;
};

class ServerEngine {
public:
    // `backend` overrides config.usb.backend (tests inject a LoopbackBackend).
    ServerEngine(ServerConfig cfg, std::unique_ptr<IUsbBackend> backend = nullptr);
    ~ServerEngine();
    ServerEngine(const ServerEngine&) = delete;
    ServerEngine& operator=(const ServerEngine&) = delete;

    Status start();
    void stop();
    [[nodiscard]] bool running() const noexcept { return running_; }

    // ---- management (thread-safe; used by REST API, GUI, CLI) ----
    [[nodiscard]] std::vector<DeviceInfo> devices() const;
    [[nodiscard]] std::vector<ClientSnapshot> clients() const;
    [[nodiscard]] nlohmann::json sessions_json() const;
    [[nodiscard]] nlohmann::json status_json() const;
    [[nodiscard]] nlohmann::json metrics_json() const;
    Status share(const std::string& device_id, bool shared);
    Status reset_device(const std::string& device_id);
    Status detach_device(const std::string& device_id, const std::string& reason = "detached by administrator");
    void rescan_now();

    [[nodiscard]] const ServerConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] const std::string& fingerprint() const noexcept { return fingerprint_; }
    [[nodiscard]] const std::string& server_id() const noexcept { return server_id_; }
    TokenStore& tokens() noexcept { return tokens_; }
    LicenseManager& license() noexcept { return license_; }
    IUsbBackend& backend() noexcept { return *backend_; }
    asio::io_context& io() noexcept { return rt_->context(); }

    // Events for the GUI: kind = "device", "client", "session", "status".
    using EventFn = std::function<void(const std::string& kind, const nlohmann::json& body)>;
    void set_event_handler(EventFn fn);

private:
    friend class ClientConn;

    struct DeviceEntry {
        DeviceInfo info;
        EnumeratedDevice raw;
        std::shared_ptr<ServerSession> session;     // owner (exclusive)
        std::vector<std::weak_ptr<ServerSession>> observers;  // shared read-only
    };

    // connection handling
    void on_accept(net::TransportPtr t);
    void remove_connection(const std::string& conn_id);

    // device registry
    void schedule_rescan(std::uint32_t delay_ms = 50);
    void do_rescan();
    void apply_scan(std::vector<EnumeratedDevice> scan);
    DeviceInfo make_info(const EnumeratedDevice& e) const;
    bool is_initially_shared(const DeviceInfo& d) const;
    void try_rebind_orphans(const std::vector<std::string>& arrived_ids);

    // sessions
    void attach(const std::shared_ptr<ClientConn>& cc, std::uint32_t corr, const std::string& device_id,
                AccessMode access, SessionPriority prio, bool mobile_repair);
    void resume(const std::shared_ptr<ClientConn>& cc, std::uint32_t corr, const std::string& session_id,
                const std::string& token);
    Result<std::shared_ptr<ServerSession>> find_session(const std::string& id);
    std::size_t count_clients_except(const std::string& conn_id, const std::string& client_id) const;
    DeviceExecutor device_executor(const std::string& port_path);
    void close_session(const std::shared_ptr<ServerSession>& s, const std::string& reason);
    void on_session_connection_lost(const std::shared_ptr<ServerSession>& s);

    // broadcasting
    void broadcast_device_event(const std::string& event, const DeviceInfo& d);
    void emit(const std::string& kind, const nlohmann::json& body);
    bool client_may_see(const TokenIdentity& tok, const DeviceInfo& d) const;

    void sample_metrics();

    ServerConfig cfg_;
    std::unique_ptr<IUsbBackend> backend_;
    std::unique_ptr<net::IoRuntime> rt_;
    std::unique_ptr<asio::thread_pool> blocking_;
    std::shared_ptr<net::Listener> listener_;
    std::shared_ptr<asio::ssl::context> tls_;
    std::unique_ptr<asio::steady_timer> poll_timer_;
    std::unique_ptr<asio::steady_timer> metrics_timer_;
    std::unique_ptr<asio::steady_timer> rescan_timer_;
    std::unique_ptr<net::Strand> registry_strand_;
    TokenStore tokens_;
    LicenseManager license_;
    ModeDetector detector_;
    IdentityMatcher matcher_;
    std::string fingerprint_;
    std::string server_id_;
    std::atomic<bool> running_{false};
    std::atomic<bool> rescan_pending_{false};
    std::atomic<bool> rescan_running_{false};
    std::atomic<std::uint32_t> next_handle_{1};
    std::atomic<std::uint64_t> next_conn_{1};
    std::int64_t started_ms_ = 0;

    mutable std::mutex m_;
    std::map<std::string, DeviceEntry> devices_;                    // present devices by id
    std::map<std::string, std::shared_ptr<ServerSession>> sessions_; // all live sessions
    std::map<std::string, std::shared_ptr<ClientConn>> conns_;
    std::map<std::string, DeviceExecutor> dev_exec_;               // per physical port
    UsbMetrics usb_totals_;  // aggregated rates sampled from sessions
    std::uint64_t total_reconnects_ = 0;
    std::uint64_t total_reenumerations_ = 0;
    CpuMeter cpu_;
    double cpu_percent_ = 0;

    std::mutex ev_m_;
    EventFn on_event_;
};

}  // namespace pf::server
