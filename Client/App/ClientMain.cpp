// parsflash-client: headless ParsFlash USB Client.
//
//   parsflash-client [--config FILE] [--test-vhci]                run and keep sessions alive
//   parsflash-client [--config FILE] --list [--test-vhci]         show remote devices and exit
//   parsflash-client [--config FILE] --add-server NAME HOST PORT [--no-tls]
//                    [--fingerprint FP] [--auto-attach PATTERN]   register a server, exit
//   parsflash-client [--config FILE] --set-token NAME TOKEN       store a token, exit
//                                                                  (TOKEN "-" reads stdin)
//
// --test-vhci uses the TEST driver double instead of the Windows virtual USB controller.
// Devices attached through it are not visible to Windows applications.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "Common/Config.hpp"
#include "Common/Platform.hpp"
#include "Core/ClientEngine.hpp"
#include "Crypto/SecretStore.hpp"
#include "Logging/Logger.hpp"
#include "VirtualUSB/TestVhci.hpp"
#include "VirtualUSB/VirtualHci.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

void usage() {
    std::cout << "ParsFlash USB Client " << PF_VERSION_STRING << "\n"
              << "usage:\n"
              << "  parsflash-client [--config FILE] [--test-vhci]\n"
              << "  parsflash-client [--config FILE] --list [--test-vhci]\n"
              << "  parsflash-client [--config FILE] --add-server NAME HOST PORT [--no-tls]\n"
              << "                   [--fingerprint FP] [--auto-attach PATTERN]\n"
              << "  parsflash-client [--config FILE] --set-token NAME TOKEN   (TOKEN \"-\" reads stdin)\n";
}

void setup_logging(const pf::ClientConfig& c) {
    pf::LogLevel level = pf::LogLevel::Info;
    pf::parse_log_level(c.log_level, level);
    auto& lg = pf::Logger::instance();
    lg.set_level(level);
    lg.add_sink(std::make_shared<pf::ConsoleSink>());
    lg.add_sink(std::make_shared<pf::RotatingFileSink>(std::filesystem::path(c.log_dir) / "client.log"));
}

// Waits until some server is connected and has reported its devices, or the timeout expires.
bool wait_for_devices(pf::client::ClientEngine& engine, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!g_stop && std::chrono::steady_clock::now() < deadline) {
        for (const auto& s : engine.servers()) {
            if (s.state == pf::client::LinkState::Connected && s.devices > 0) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path config_path = pf::app_data_dir("client") / "config.json";
    bool list = false;
    bool test_vhci = false;
    std::optional<pf::ServerEntry> add_server;
    std::optional<std::pair<std::string, std::string>> set_token;
    std::optional<std::string> fingerprint;
    std::optional<std::string> auto_attach;
    bool no_tls = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](int n) { return i + n < argc; };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--config" && need(1)) {
            config_path = argv[++i];
        } else if (a == "--list") {
            list = true;
        } else if (a == "--test-vhci") {
            test_vhci = true;
        } else if (a == "--no-tls") {
            no_tls = true;
        } else if (a == "--fingerprint" && need(1)) {
            fingerprint = argv[++i];
        } else if (a == "--auto-attach" && need(1)) {
            auto_attach = argv[++i];
        } else if (a == "--add-server" && need(3)) {
            pf::ServerEntry e;
            e.name = argv[i + 1];
            e.host = argv[i + 2];
            e.port = static_cast<std::uint16_t>(std::atoi(argv[i + 3]));
            i += 3;
            add_server = e;
        } else if (a == "--set-token" && need(2)) {
            set_token = std::make_pair(std::string(argv[i + 1]), std::string(argv[i + 2]));
            i += 2;
        } else {
            std::cerr << "unknown or incomplete option: " << a << "\n";
            usage();
            return 2;
        }
    }

    auto loaded = pf::load_client_config(config_path);
    if (!loaded) {
        std::cerr << "configuration error: " << loaded.error().describe() << "\n";
        return 1;
    }
    pf::ClientConfig cfg = *loaded;
    setup_logging(cfg);
    const auto data_dir = config_path.parent_path();
    pf::SecretStore secrets(data_dir / "secrets.json");
    if (auto st = secrets.load(); !st) {
        std::cerr << st.error().describe() << "\n";
        return 1;
    }

    // ---- configuration commands (no network) ----
    bool changed = false;
    if (set_token) {
        std::string token = set_token->second;
        if (token == "-") std::getline(std::cin, token);
        auto it = std::find_if(cfg.servers.begin(), cfg.servers.end(),
                               [&](const pf::ServerEntry& e) { return e.name == set_token->first; });
        if (it == cfg.servers.end()) {
            std::cerr << "no server named '" << set_token->first << "' (use --add-server first)\n";
            return 1;
        }
        it->token_key = "server:" + it->name;
        if (auto st = secrets.put(it->token_key, token); !st) {
            std::cerr << "cannot store token: " << st.error().describe() << "\n";
            return 1;
        }
        changed = true;
        std::cout << "token stored for '" << it->name << "'\n";
    }
    if (add_server) {
        if (std::any_of(cfg.servers.begin(), cfg.servers.end(),
                        [&](const pf::ServerEntry& e) { return e.name == add_server->name; })) {
            std::cerr << "server '" << add_server->name << "' already exists\n";
            return 1;
        }
        add_server->tls = !no_tls;
        if (fingerprint) add_server->fingerprint = *fingerprint;
        if (auto_attach) add_server->auto_attach.push_back(*auto_attach);
        cfg.servers.push_back(*add_server);
        changed = true;
        std::cout << "server '" << add_server->name << "' added\n";
    }
    if (changed) {
        if (auto st = pf::save_config(config_path, pf::to_json(cfg)); !st) {
            std::cerr << "cannot save configuration: " << st.error().describe() << "\n";
            return 1;
        }
    }
    if (set_token || add_server) return changed ? 0 : 1;

    // ---- run ----
    std::unique_ptr<pf::client::IVirtualHci> vhci;
#ifdef _WIN32
    if (!test_vhci) vhci = pf::client::make_windows_vhci(cfg.vhci);
#endif
    if (test_vhci) {
        std::cout << "WARNING: --test-vhci: devices are NOT exposed to Windows applications\n";
        vhci = std::make_unique<pf::client::TestVhci>();
    }
    auto token_for = [&secrets](const pf::ServerEntry& e) {
        auto r = secrets.get(e.token_key);
        return r ? *r : std::string{};
    };
    pf::client::ClientEngine engine(cfg, std::move(vhci), token_for);
    if (auto st = engine.start(); !st) {
        std::cerr << "cannot start client: " << st.error().describe() << "\n";
        return 1;
    }
    if (!engine.vhci_available()) std::cout << "virtual USB controller: " << engine.vhci_error() << "\n";

    if (list) {
        const bool found = wait_for_devices(engine, std::chrono::seconds(15));
        for (const auto& s : engine.servers()) {
            std::cout << "server " << s.name << " (" << s.host << ":" << s.port << "): " << pf::client::to_string(s.state);
            if (!s.last_error.empty()) std::cout << " - " << s.last_error;
            std::cout << "\n";
            for (const auto& d : engine.devices(s.name)) {
                std::cout << "  " << d.id << "  " << pf::format_vid_pid(d.identity.vid, d.identity.pid) << "  "
                          << d.display_name() << "  mode=" << d.mode.label << "  " << pf::to_string(d.state) << "\n";
            }
        }
        engine.stop();
        pf::Logger::instance().flush();
        return found ? 0 : 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::cout << "client '" << cfg.client_name << "' running - press Ctrl+C to stop" << std::endl;
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "stopping..." << std::endl;
    engine.stop();
    pf::Logger::instance().flush();
    return 0;
}
