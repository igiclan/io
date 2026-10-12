// End-to-end harness: real ServerEngine (loopback TEST backend) + real ClientEngine
// (TestVhci driver double), connected over loopback TCP with TLS 1.3 and token auth.
#pragma once

#include <memory>

#include "../Common/TestEnv.hpp"
#include "Logging/Logger.hpp"
#include "Core/ClientEngine.hpp"
#include "Core/ServerEngine.hpp"
#include "USB/LoopbackBackend.hpp"
#include "VirtualUSB/TestVhci.hpp"

namespace pf::test {

struct HarnessOptions {
    std::uint32_t devices = 2;
    bool tls = true;
    std::uint32_t reconnect_timeout_ms = 5000;
    std::uint32_t keepalive_ms = 200;
    bool share_all = true;
    bool api = false;  // management API on an ephemeral port
};

class Harness {
public:
    explicit Harness(HarnessOptions o = {}) : opts_(o) {
        auto& lg = Logger::instance();
        lg.set_level(LogLevel::Warning);
        ServerConfig sc;
        apply_defaults(sc, dir_.path() / "server");
        sc.listen_address = "127.0.0.1";
        sc.server_port = 0;  // ephemeral
        sc.api.enabled = o.api;
        sc.api.port = 0;
        sc.tls_enabled = o.tls;
        sc.reconnect_timeout_ms = o.reconnect_timeout_ms;
        sc.keepalive.interval_ms = o.keepalive_ms;
        sc.keepalive.miss_limit = 3;
        sc.usb.share_policy = o.share_all ? "all" : "manual";
        sc.max_clients = 8;
        auto be = std::make_unique<server::LoopbackBackend>(o.devices);
        backend = be.get();
        server = std::make_unique<server::ServerEngine>(sc, std::move(be));
        auto st = server->start();
        if (!st) throw std::runtime_error(st.error().describe());
        auto tok = server->tokens().create("integration", true);  // admin: tests drive every endpoint
        if (!tok) throw std::runtime_error(tok.error().describe());
        token = *tok;
    }
    ~Harness() {
        for (auto& c : clients) c.engine->stop();
        clients.clear();
        server->stop();
    }

    struct ClientHandle {
        std::unique_ptr<client::ClientEngine> engine;
        client::TestVhci* vhci;
    };

    // Creates and starts a client connected to the server.
    client::ClientEngine& add_client(const std::string& name, std::string tok = {}) {
        ClientConfig cc;
        cc.client_name = name;
        cc.keepalive.interval_ms = opts_.keepalive_ms;
        cc.keepalive.miss_limit = 3;
        cc.reconnect_timeout_ms = opts_.reconnect_timeout_ms;
        cc.reconnect.initial_delay_ms = 50;
        cc.reconnect.max_delay_ms = 200;
        ServerEntry e;
        e.name = "bench";
        e.host = "127.0.0.1";
        e.port = server->port();
        e.tls = opts_.tls;
        cc.servers.push_back(e);
        auto vh = std::make_unique<client::TestVhci>();
        auto* raw = vh.get();
        if (tok.empty()) tok = token;
        auto eng = std::make_unique<client::ClientEngine>(cc, std::move(vh),
                                                          [tok](const ServerEntry&) { return tok; });
        auto st = eng->start();
        if (!st) throw std::runtime_error(st.error().describe());
        clients.push_back(ClientHandle{std::move(eng), raw});
        return *clients.back().engine;
    }

    client::TestVhci& vhci(std::size_t i = 0) { return *clients.at(i).vhci; }

    bool wait_connected(client::ClientEngine& c, std::chrono::milliseconds t = std::chrono::seconds(10)) {
        return wait_for([&] {
            auto s = c.servers();
            return !s.empty() && s[0].state == client::LinkState::Connected && s[0].devices >= opts_.devices;
        }, t);
    }

    // Device id of the emulated device on port "1-<n>"; waits for the server's
    // (asynchronous) USB scan to have seen it.
    std::string device_on_port(int n, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::string id;
        wait_for([&] {
            for (const auto& d : server->devices())
                if (d.identity.port_path == "1-" + std::to_string(n)) id = d.id;
            return !id.empty();
        }, timeout);
        return id;
    }

    HarnessOptions opts_;
    TempDir dir_;
    server::LoopbackBackend* backend = nullptr;
    std::unique_ptr<server::ServerEngine> server;
    std::string token;
    std::vector<ClientHandle> clients;
};

}  // namespace pf::test
