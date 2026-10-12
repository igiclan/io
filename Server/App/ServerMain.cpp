// parsflash-server: headless ParsFlash USB Server.
//
//   parsflash-server [--config FILE] [--test-devices N]
//   parsflash-server [--config FILE] --create-token NAME [--admin]
//
// Runs until Ctrl+C / SIGTERM. --create-token prints a new client token once and exits.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include "Common/Config.hpp"
#include "Common/Platform.hpp"
#include "Core/ServerEngine.hpp"
#include "Crypto/TokenStore.hpp"
#include "Logging/Logger.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

void usage() {
    std::cout << "ParsFlash USB Server " << PF_VERSION_STRING << "\n"
              << "usage:\n"
              << "  parsflash-server [--config FILE] [--test-devices N]\n"
              << "  parsflash-server [--config FILE] --create-token NAME [--admin]\n"
              << "options:\n"
              << "  --config FILE      configuration file (default: <data>/server/config.json)\n"
              << "  --test-devices N   use N emulated TEST devices instead of real USB (testing only)\n"
              << "  --create-token N   create a client token named N, print it once, exit\n"
              << "  --admin            the new token may change server state (with --create-token)\n";
}

void setup_logging(const pf::ServerConfig& c) {
    pf::LogLevel level = pf::LogLevel::Info;
    pf::parse_log_level(c.log_level, level);
    auto& lg = pf::Logger::instance();
    lg.set_level(level);
    lg.add_sink(std::make_shared<pf::ConsoleSink>());
    lg.add_sink(std::make_shared<pf::RotatingFileSink>(std::filesystem::path(c.log_dir) / "server.log"));
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path config_path = pf::app_data_dir("server") / "config.json";
    std::optional<std::string> create_token;
    bool admin = false;
    std::optional<std::uint32_t> test_devices;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_next = i + 1 < argc;
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--config" && has_next) {
            config_path = argv[++i];
        } else if (a == "--create-token" && has_next) {
            create_token = argv[++i];
        } else if (a == "--admin") {
            admin = true;
        } else if (a == "--test-devices" && has_next) {
            test_devices = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::cerr << "unknown or incomplete option: " << a << "\n";
            usage();
            return 2;
        }
    }

    auto loaded = pf::load_server_config(config_path);
    if (!loaded) {
        std::cerr << "configuration error: " << loaded.error().describe() << "\n";
        return 1;
    }
    pf::ServerConfig cfg = *loaded;
    if (test_devices) cfg.usb.test_devices = *test_devices;
    setup_logging(cfg);

    if (create_token) {
        pf::TokenStore store(cfg.tokens_file);
        if (auto st = store.load(); !st) {
            std::cerr << st.error().describe() << "\n";
            return 1;
        }
        auto token = store.create(*create_token, admin);
        if (!token) {
            std::cerr << "cannot create token: " << token.error().describe() << "\n";
            return 1;
        }
        std::cout << "token for '" << *create_token << "' (shown once, store it safely):\n" << *token << "\n";
        pf::Logger::instance().flush();
        return 0;
    }

    pf::server::ServerEngine engine(cfg);
    if (auto st = engine.start(); !st) {
        pf::Logger::instance().flush();
        std::cerr << "cannot start server: " << st.error().describe() << "\n";
        return 1;
    }
    if (cfg.tls_enabled) std::cout << "certificate fingerprint: " << engine.fingerprint() << "\n";
    std::cout << "running on port " << engine.port() << " - press Ctrl+C to stop" << std::endl;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "stopping..." << std::endl;
    engine.stop();
    pf::Logger::instance().flush();
    return 0;
}
