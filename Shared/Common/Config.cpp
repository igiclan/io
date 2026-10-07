#include "Common/Config.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

#include "Common/Platform.hpp"

namespace pf {

using nlohmann::json;

std::string_view to_string(PerformanceMode m) noexcept {
    switch (m) {
        case PerformanceMode::Balanced: return "balanced";
        case PerformanceMode::LowLatency: return "low-latency";
        case PerformanceMode::Throughput: return "throughput";
        case PerformanceMode::PowerSaver: return "power-saver";
    }
    return "balanced";
}

bool parse_performance_mode(std::string_view s, PerformanceMode& out) noexcept {
    for (auto m : {PerformanceMode::Balanced, PerformanceMode::LowLatency, PerformanceMode::Throughput,
                   PerformanceMode::PowerSaver}) {
        if (to_string(m) == s) {
            out = m;
            return true;
        }
    }
    return false;
}

std::uint32_t effective_worker_threads(std::uint32_t configured) noexcept {
    if (configured != 0) return configured;
    return std::min(4u, hardware_threads());
}

// ---------------------------------------------------------------- helpers

namespace {

json keepalive_json(const KeepaliveConfig& k) {
    return {{"interval_ms", k.interval_ms}, {"miss_limit", k.miss_limit}, {"tcp_keepalive", k.tcp_keepalive}};
}
void keepalive_from(const json& j, KeepaliveConfig& k) {
    if (!j.is_object()) return;
    k.interval_ms = j.value("interval_ms", k.interval_ms);
    k.miss_limit = j.value("miss_limit", k.miss_limit);
    k.tcp_keepalive = j.value("tcp_keepalive", k.tcp_keepalive);
}

json network_json(const NetworkTuning& n) {
    return {{"read_buffer_size", n.read_buffer_size},       {"socket_buffer_size", n.socket_buffer_size},
            {"max_frame_size", n.max_frame_size},           {"send_high_watermark", n.send_high_watermark},
            {"send_low_watermark", n.send_low_watermark},   {"max_write_batch", n.max_write_batch},
            {"tcp_nodelay", n.tcp_nodelay}};
}
void network_from(const json& j, NetworkTuning& n) {
    if (!j.is_object()) return;
    n.read_buffer_size = j.value("read_buffer_size", n.read_buffer_size);
    n.socket_buffer_size = j.value("socket_buffer_size", n.socket_buffer_size);
    n.max_frame_size = j.value("max_frame_size", n.max_frame_size);
    n.send_high_watermark = j.value("send_high_watermark", n.send_high_watermark);
    n.send_low_watermark = j.value("send_low_watermark", n.send_low_watermark);
    n.max_write_batch = j.value("max_write_batch", n.max_write_batch);
    n.tcp_nodelay = j.value("tcp_nodelay", n.tcp_nodelay);
}

Status perf_from(const json& j, const char* key, PerformanceMode& m) {
    if (!j.contains(key)) return {};
    if (!parse_performance_mode(j.at(key).get<std::string>(), m))
        return {ErrorCode::InvalidArgument, std::string("invalid ") + key};
    return {};
}

}  // namespace

// ---------------------------------------------------------------- server

json to_json(const ServerConfig& c) {
    return {{"server_name", c.server_name},
            {"listen_address", c.listen_address},
            {"server_port", c.server_port},
            {"tls_enabled", c.tls_enabled},
            {"cert_file", c.cert_file},
            {"key_file", c.key_file},
            {"authentication", c.authentication},
            {"tokens_file", c.tokens_file},
            {"max_clients", c.max_clients},
            {"max_devices", c.max_devices},
            {"buffer_size", c.buffer_size},
            {"worker_threads", c.worker_threads},
            {"keepalive", keepalive_json(c.keepalive)},
            {"reconnect_timeout", c.reconnect_timeout_ms},
            {"auth_timeout", c.auth_timeout_ms},
            {"log_level", c.log_level},
            {"log_dir", c.log_dir},
            {"performance_mode", to_string(c.performance_mode)},
            {"mobile_repair_mode", c.mobile_repair_mode},
            {"network", network_json(c.network)},
            {"api", {{"enabled", c.api.enabled}, {"bind_address", c.api.bind_address}, {"port", c.api.port}}},
            {"usb",
             {{"backend", c.usb.backend},
              {"use_usbdk", c.usb.use_usbdk},
              {"poll_interval_ms", c.usb.poll_interval_ms},
              {"auto_capture_on_reenumeration", c.usb.auto_capture_on_reenumeration},
              {"test_devices", c.usb.test_devices},
              {"share_policy", c.usb.share_policy},
              {"shared", c.usb.shared},
              {"max_inflight_per_endpoint", c.usb.max_inflight_per_endpoint}}},
            {"license_file", c.license_file},
            {"start_with_windows", c.start_with_windows},
            {"language", c.language},
            {"theme", c.theme}};
}

Status from_json(const json& j, ServerConfig& c) {
    if (!j.is_object()) return {ErrorCode::InvalidArgument, "config must be a JSON object"};
    try {
        c.server_name = j.value("server_name", c.server_name);
        c.listen_address = j.value("listen_address", c.listen_address);
        c.server_port = j.value("server_port", c.server_port);
        c.tls_enabled = j.value("tls_enabled", c.tls_enabled);
        c.cert_file = j.value("cert_file", c.cert_file);
        c.key_file = j.value("key_file", c.key_file);
        c.authentication = j.value("authentication", c.authentication);
        c.tokens_file = j.value("tokens_file", c.tokens_file);
        c.max_clients = j.value("max_clients", c.max_clients);
        c.max_devices = j.value("max_devices", c.max_devices);
        c.buffer_size = j.value("buffer_size", c.buffer_size);
        c.worker_threads = j.value("worker_threads", c.worker_threads);
        if (j.contains("keepalive")) keepalive_from(j["keepalive"], c.keepalive);
        c.reconnect_timeout_ms = j.value("reconnect_timeout", c.reconnect_timeout_ms);
        c.auth_timeout_ms = j.value("auth_timeout", c.auth_timeout_ms);
        c.log_level = j.value("log_level", c.log_level);
        c.log_dir = j.value("log_dir", c.log_dir);
        if (auto st = perf_from(j, "performance_mode", c.performance_mode); !st) return st;
        c.mobile_repair_mode = j.value("mobile_repair_mode", c.mobile_repair_mode);
        if (j.contains("network")) network_from(j["network"], c.network);
        if (j.contains("buffer_size") && !(j.contains("network") && j["network"].contains("read_buffer_size")))
            c.network.read_buffer_size = c.buffer_size;
        if (auto it = j.find("api"); it != j.end() && it->is_object()) {
            c.api.enabled = it->value("enabled", c.api.enabled);
            c.api.bind_address = it->value("bind_address", c.api.bind_address);
            c.api.port = it->value("port", c.api.port);
        }
        if (auto it = j.find("usb"); it != j.end() && it->is_object()) {
            c.usb.backend = it->value("backend", c.usb.backend);
            c.usb.use_usbdk = it->value("use_usbdk", c.usb.use_usbdk);
            c.usb.poll_interval_ms = it->value("poll_interval_ms", c.usb.poll_interval_ms);
            c.usb.auto_capture_on_reenumeration =
                it->value("auto_capture_on_reenumeration", c.usb.auto_capture_on_reenumeration);
            c.usb.test_devices = it->value("test_devices", c.usb.test_devices);
            c.usb.share_policy = it->value("share_policy", c.usb.share_policy);
            c.usb.shared = it->value("shared", c.usb.shared);
            c.usb.max_inflight_per_endpoint = it->value("max_inflight_per_endpoint", c.usb.max_inflight_per_endpoint);
        }
        c.license_file = j.value("license_file", c.license_file);
        c.start_with_windows = j.value("start_with_windows", c.start_with_windows);
        c.language = j.value("language", c.language);
        c.theme = j.value("theme", c.theme);
    } catch (const json::exception& e) {
        return {ErrorCode::InvalidArgument, std::string("config: ") + e.what()};
    }
    return validate(c);
}

Status validate(const ServerConfig& c) {
    if (c.server_port == 0) return {ErrorCode::InvalidArgument, "server_port must be 1..65535"};
    if (c.max_clients == 0 || c.max_clients > 1024) return {ErrorCode::InvalidArgument, "max_clients 1..1024"};
    if (c.max_devices == 0 || c.max_devices > 1024) return {ErrorCode::InvalidArgument, "max_devices 1..1024"};
    if (c.worker_threads > 256) return {ErrorCode::InvalidArgument, "worker_threads 0..256"};
    if (c.reconnect_timeout_ms > 24u * 3600 * 1000) return {ErrorCode::InvalidArgument, "reconnect_timeout too big"};
    if (c.keepalive.interval_ms < 100) return {ErrorCode::InvalidArgument, "keepalive.interval_ms >= 100"};
    if (c.keepalive.miss_limit == 0) return {ErrorCode::InvalidArgument, "keepalive.miss_limit >= 1"};
    if (c.network.read_buffer_size < 4096) return {ErrorCode::InvalidArgument, "read buffer >= 4096"};
    if (c.network.max_frame_size < 64 * 1024) return {ErrorCode::InvalidArgument, "max_frame_size >= 64 KiB"};
    if (c.network.send_low_watermark >= c.network.send_high_watermark)
        return {ErrorCode::InvalidArgument, "send_low_watermark must be < send_high_watermark"};
    if (c.api.enabled && c.api.port == c.server_port) return {ErrorCode::InvalidArgument, "api.port == server_port"};
    if (c.usb.backend != "auto" && c.usb.backend != "libusb" && c.usb.backend != "test")
        return {ErrorCode::InvalidArgument, "usb.backend must be auto|libusb|test"};
    if (c.usb.share_policy != "manual" && c.usb.share_policy != "all")
        return {ErrorCode::InvalidArgument, "usb.share_policy must be manual|all"};
    if (c.usb.max_inflight_per_endpoint == 0) return {ErrorCode::InvalidArgument, "max_inflight_per_endpoint >= 1"};
    static const char* kLevels[] = {"trace", "debug", "info", "warning", "warn", "error", "critical", "off"};
    if (std::none_of(std::begin(kLevels), std::end(kLevels), [&](const char* l) { return c.log_level == l; }))
        return {ErrorCode::InvalidArgument, "invalid log_level"};
    return {};
}

void apply_defaults(ServerConfig& c, const std::filesystem::path& data) {
    if (c.server_name.empty()) c.server_name = host_name();
    if (c.cert_file.empty()) c.cert_file = (data / "server.crt").string();
    if (c.key_file.empty()) c.key_file = (data / "server.key").string();
    if (c.tokens_file.empty()) c.tokens_file = (data / "tokens.json").string();
    if (c.log_dir.empty()) c.log_dir = (data / "logs").string();
    if (c.license_file.empty()) c.license_file = (data / "license.json").string();
}

// ---------------------------------------------------------------- client

json to_json(const ClientConfig& c) {
    json servers = json::array();
    for (const auto& s : c.servers) {
        servers.push_back({{"name", s.name},
                           {"host", s.host},
                           {"port", s.port},
                           {"tls", s.tls},
                           {"fingerprint", s.fingerprint},
                           {"token_key", s.token_key},
                           {"auto_connect", s.auto_connect},
                           {"auto_attach", s.auto_attach}});
    }
    return {{"client_name", c.client_name},
            {"client_id", c.client_id},
            {"servers", servers},
            {"auto_reconnect", c.auto_reconnect},
            {"reconnect",
             {{"initial_delay_ms", c.reconnect.initial_delay_ms},
              {"max_delay_ms", c.reconnect.max_delay_ms},
              {"multiplier", c.reconnect.multiplier}}},
            {"reconnect_timeout", c.reconnect_timeout_ms},
            {"keepalive", keepalive_json(c.keepalive)},
            {"worker_threads", c.worker_threads},
            {"log_level", c.log_level},
            {"log_dir", c.log_dir},
            {"performance_mode", to_string(c.performance_mode)},
            {"mobile_repair_mode", c.mobile_repair_mode},
            {"default_priority", c.default_priority},
            {"network", network_json(c.network)},
            {"vhci", {{"fetch_threads", c.vhci.fetch_threads}, {"fetch_buffer_size", c.vhci.fetch_buffer_size}}},
            {"start_with_windows", c.start_with_windows},
            {"language", c.language},
            {"theme", c.theme}};
}

Status from_json(const json& j, ClientConfig& c) {
    if (!j.is_object()) return {ErrorCode::InvalidArgument, "config must be a JSON object"};
    try {
        c.client_name = j.value("client_name", c.client_name);
        c.client_id = j.value("client_id", c.client_id);
        if (auto it = j.find("servers"); it != j.end() && it->is_array()) {
            c.servers.clear();
            for (const auto& s : *it) {
                ServerEntry e;
                e.name = s.value("name", "");
                e.host = s.at("host").get<std::string>();
                e.port = s.value("port", e.port);
                e.tls = s.value("tls", e.tls);
                e.fingerprint = s.value("fingerprint", "");
                e.token_key = s.value("token_key", "");
                e.auto_connect = s.value("auto_connect", e.auto_connect);
                e.auto_attach = s.value("auto_attach", std::vector<std::string>{});
                if (e.name.empty()) e.name = e.host;
                c.servers.push_back(std::move(e));
            }
        }
        c.auto_reconnect = j.value("auto_reconnect", c.auto_reconnect);
        if (auto it = j.find("reconnect"); it != j.end() && it->is_object()) {
            c.reconnect.initial_delay_ms = it->value("initial_delay_ms", c.reconnect.initial_delay_ms);
            c.reconnect.max_delay_ms = it->value("max_delay_ms", c.reconnect.max_delay_ms);
            c.reconnect.multiplier = it->value("multiplier", c.reconnect.multiplier);
        }
        c.reconnect_timeout_ms = j.value("reconnect_timeout", c.reconnect_timeout_ms);
        if (j.contains("keepalive")) keepalive_from(j["keepalive"], c.keepalive);
        c.worker_threads = j.value("worker_threads", c.worker_threads);
        c.log_level = j.value("log_level", c.log_level);
        c.log_dir = j.value("log_dir", c.log_dir);
        if (auto st = perf_from(j, "performance_mode", c.performance_mode); !st) return st;
        c.mobile_repair_mode = j.value("mobile_repair_mode", c.mobile_repair_mode);
        c.default_priority = j.value("default_priority", c.default_priority);
        if (j.contains("network")) network_from(j["network"], c.network);
        if (auto it = j.find("vhci"); it != j.end() && it->is_object()) {
            c.vhci.fetch_threads = it->value("fetch_threads", c.vhci.fetch_threads);
            c.vhci.fetch_buffer_size = it->value("fetch_buffer_size", c.vhci.fetch_buffer_size);
        }
        c.start_with_windows = j.value("start_with_windows", c.start_with_windows);
        c.language = j.value("language", c.language);
        c.theme = j.value("theme", c.theme);
    } catch (const json::exception& e) {
        return {ErrorCode::InvalidArgument, std::string("config: ") + e.what()};
    }
    return validate(c);
}

Status validate(const ClientConfig& c) {
    for (const auto& s : c.servers) {
        if (s.host.empty()) return {ErrorCode::InvalidArgument, "server host empty"};
        if (s.port == 0) return {ErrorCode::InvalidArgument, "server port invalid"};
    }
    if (c.reconnect.initial_delay_ms == 0 || c.reconnect.max_delay_ms < c.reconnect.initial_delay_ms)
        return {ErrorCode::InvalidArgument, "reconnect delays invalid"};
    if (c.reconnect.multiplier < 1.0) return {ErrorCode::InvalidArgument, "reconnect.multiplier >= 1"};
    if (c.keepalive.interval_ms < 100 || c.keepalive.miss_limit == 0)
        return {ErrorCode::InvalidArgument, "keepalive invalid"};
    if (c.vhci.fetch_threads == 0 || c.vhci.fetch_threads > 64) return {ErrorCode::InvalidArgument, "fetch_threads"};
    if (c.vhci.fetch_buffer_size < 64 * 1024) return {ErrorCode::InvalidArgument, "fetch_buffer_size >= 64 KiB"};
    if (c.network.send_low_watermark >= c.network.send_high_watermark)
        return {ErrorCode::InvalidArgument, "send watermarks"};
    if (c.default_priority != "low" && c.default_priority != "normal" && c.default_priority != "high" &&
        c.default_priority != "critical")
        return {ErrorCode::InvalidArgument, "default_priority"};
    return {};
}

void apply_defaults(ClientConfig& c, const std::filesystem::path& data) {
    if (c.client_name.empty()) c.client_name = host_name();
    if (c.log_dir.empty()) c.log_dir = (data / "logs").string();
}

// ---------------------------------------------------------------- files

Status save_config(const std::filesystem::path& file, const json& j) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return {ErrorCode::Io, "cannot write " + tmp.string()};
        out << j.dump(2) << '\n';
        if (!out) return {ErrorCode::Io, "write failed " + tmp.string()};
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) return {ErrorCode::Io, "rename failed: " + ec.message()};
    return {};
}

template <typename Cfg>
static Result<Cfg> load_cfg(const std::filesystem::path& file) {
    Cfg c;
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        std::ifstream in(file);
        json j = json::parse(in, nullptr, false, true /* allow comments */);
        if (j.is_discarded()) return Error{ErrorCode::InvalidArgument, "invalid JSON in " + file.string()};
        if (auto st = from_json(j, c); !st) return st.error();
    }
    apply_defaults(c, file.parent_path());
    if (!std::filesystem::exists(file, ec)) {
        if (auto st = save_config(file, to_json(c)); !st) return st.error();
    }
    return c;
}

Result<ServerConfig> load_server_config(const std::filesystem::path& file) { return load_cfg<ServerConfig>(file); }
Result<ClientConfig> load_client_config(const std::filesystem::path& file) { return load_cfg<ClientConfig>(file); }

}  // namespace pf
