// Configuration for Server and Client (config.json). All tunables live here; no
// component hard-codes these values. Unknown keys are preserved on save is NOT
// guaranteed; the file is rewritten from the struct.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "Common/Error.hpp"

namespace pf {

enum class PerformanceMode : std::uint8_t { Balanced = 0, LowLatency = 1, Throughput = 2, PowerSaver = 3 };
std::string_view to_string(PerformanceMode m) noexcept;
bool parse_performance_mode(std::string_view s, PerformanceMode& out) noexcept;

struct KeepaliveConfig {
    std::uint32_t interval_ms = 5000;
    std::uint32_t miss_limit = 3;
    bool tcp_keepalive = true;
};

struct NetworkTuning {
    std::uint32_t read_buffer_size = 256 * 1024;   // staging buffer for the frame decoder
    std::uint32_t socket_buffer_size = 0;          // SO_SNDBUF/SO_RCVBUF, 0 = OS default (autotuning)
    std::uint32_t max_frame_size = 16 * 1024 * 1024 + 64 * 1024;
    std::uint32_t send_high_watermark = 32 * 1024 * 1024;
    std::uint32_t send_low_watermark = 8 * 1024 * 1024;
    std::uint32_t max_write_batch = 1024 * 1024;   // bytes gathered into one write
    bool tcp_nodelay = true;
};

struct ApiConfig {
    bool enabled = true;
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 39001;
};

struct ServerUsbConfig {
    std::string backend = "auto";        // auto | libusb | test
    bool use_usbdk = true;               // Windows: libusb UsbDk backend
    std::uint32_t poll_interval_ms = 1000;
    bool auto_capture_on_reenumeration = true;
    std::uint32_t test_devices = 0;      // number of loopback test devices (tests/benchmarks only)
    std::string share_policy = "manual"; // manual | all
    std::vector<std::string> shared;     // device ids or port paths shared at startup
    std::uint32_t max_inflight_per_endpoint = 32;
};

struct ServerConfig {
    std::string server_name;             // default: host name
    std::string listen_address = "0.0.0.0";
    std::uint16_t server_port = 39000;
    bool tls_enabled = true;
    std::string cert_file;               // default: <data>/server.crt
    std::string key_file;                // default: <data>/server.key
    bool authentication = true;          // require token AUTH
    std::string tokens_file;             // default: <data>/tokens.json
    std::uint32_t max_clients = 16;
    std::uint32_t max_devices = 32;
    std::uint32_t buffer_size = 256 * 1024;  // == network.read_buffer_size (kept for config compatibility)
    std::uint32_t worker_threads = 0;    // 0 = auto (min(4, cores))
    KeepaliveConfig keepalive;
    std::uint32_t reconnect_timeout_ms = 60000;
    std::uint32_t auth_timeout_ms = 10000;
    std::string log_level = "info";
    std::string log_dir;                 // default: <data>/logs
    PerformanceMode performance_mode = PerformanceMode::Balanced;
    bool mobile_repair_mode = true;
    NetworkTuning network;
    ApiConfig api;
    ServerUsbConfig usb;
    std::string license_file;            // default: <data>/license.json
    bool start_with_windows = false;
    std::string language = "en";         // en | fa
    std::string theme = "dark";          // dark | light | system
};

struct ServerEntry {
    std::string name;
    std::string host;
    std::uint16_t port = 39000;
    bool tls = true;
    std::string fingerprint;   // pinned server certificate SHA-256 (empty = trust on first use)
    std::string token_key;     // key into the SecretStore (token is never stored in config)
    bool auto_connect = true;
    std::vector<std::string> auto_attach;  // device ids / "vid:pid" globs attached automatically
};

struct ReconnectConfig {
    std::uint32_t initial_delay_ms = 250;
    std::uint32_t max_delay_ms = 5000;
    double multiplier = 2.0;
};

struct VhciConfig {
    std::uint32_t fetch_threads = 2;
    std::uint32_t fetch_buffer_size = 1024 * 1024;
};

struct ClientConfig {
    std::string client_name;   // default: host name
    std::string client_id;     // generated once (uuid)
    std::vector<ServerEntry> servers;
    bool auto_reconnect = true;
    ReconnectConfig reconnect;
    std::uint32_t reconnect_timeout_ms = 60000;
    KeepaliveConfig keepalive;
    std::uint32_t worker_threads = 0;
    std::string log_level = "info";
    std::string log_dir;
    PerformanceMode performance_mode = PerformanceMode::Balanced;
    bool mobile_repair_mode = true;
    std::string default_priority = "high";
    NetworkTuning network;
    VhciConfig vhci;
    bool start_with_windows = false;
    std::string language = "en";
    std::string theme = "dark";
};

nlohmann::json to_json(const ServerConfig& c);
nlohmann::json to_json(const ClientConfig& c);
Status from_json(const nlohmann::json& j, ServerConfig& c);
Status from_json(const nlohmann::json& j, ClientConfig& c);
Status validate(const ServerConfig& c);
Status validate(const ClientConfig& c);

// Fills empty paths/names with platform defaults below data_dir.
void apply_defaults(ServerConfig& c, const std::filesystem::path& data_dir);
void apply_defaults(ClientConfig& c, const std::filesystem::path& data_dir);

// Loads `file`; creates it with defaults when missing. Values are validated.
Result<ServerConfig> load_server_config(const std::filesystem::path& file);
Result<ClientConfig> load_client_config(const std::filesystem::path& file);
Status save_config(const std::filesystem::path& file, const nlohmann::json& j);

std::uint32_t effective_worker_threads(std::uint32_t configured) noexcept;

}  // namespace pf
