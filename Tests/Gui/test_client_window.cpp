// Drives the real client window against a running server engine (emulated devices) with
// the TEST driver double, using Qt's offscreen platform.
#include <doctest/doctest.h>

#include <QApplication>
#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <thread>

#include "../Common/TestEnv.hpp"
#include "Common/Config.hpp"
#include "Core/ClientEngine.hpp"
#include "Core/ServerEngine.hpp"
#include "Crypto/SecretStore.hpp"
#include "Logging/Logger.hpp"
#include "UI/ClientWindow.hpp"
#include "VirtualUSB/TestVhci.hpp"

using namespace pf;
using namespace std::chrono_literals;

namespace {

bool pump_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 8s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    QApplication::processEvents();
    return pred();
}

struct Fixture {
    test::TempDir dir;
    std::unique_ptr<server::ServerEngine> server;
    std::string token;
    SecretStore secrets{dir / "secrets.json"};
    client::TestVhci* vhci = nullptr;
    std::unique_ptr<client::ClientEngine> engine;
    std::unique_ptr<gui::ClientWindow> window;
    std::filesystem::path config_path = dir / "client" / "config.json";

    Fixture() {
        Logger::instance().set_level(LogLevel::Warning);
        ServerConfig scfg;
        apply_defaults(scfg, dir / "srv");
        scfg.listen_address = "127.0.0.1";
        scfg.server_port = 0;
        scfg.api.enabled = false;
        scfg.tls_enabled = false;
        scfg.usb.test_devices = 2;
        scfg.usb.share_policy = "all";
        server = std::make_unique<server::ServerEngine>(scfg);
        REQUIRE(server->start().ok());
        auto tok = server->tokens().create("gui-test", true);
        REQUIRE(tok.ok());
        token = *tok;

        ClientConfig ccfg;
        ccfg.client_name = "PC-TEST";
        ccfg.client_id = crypto::uuid4();
        ccfg.default_priority = "high";
        auto vh = std::make_unique<client::TestVhci>();
        vhci = vh.get();
        engine = std::make_unique<client::ClientEngine>(
            ccfg, std::move(vh), [this](const ServerEntry& e) {
                auto r = secrets.get(e.token_key);
                return r ? *r : std::string{};
            });
        REQUIRE(engine->start().ok());
        window = std::make_unique<gui::ClientWindow>(*engine, secrets, config_path, true);
        window->setConfirmations(false);
    }
    ~Fixture() {
        window.reset();
        engine->stop();
        server->stop();
    }

    ServerEntry entry() const {
        ServerEntry e;
        e.name = "bench";
        e.host = "127.0.0.1";
        e.port = server->port();
        e.tls = false;
        return e;
    }

    // Adds the server through the window and waits until its devices are listed.
    void connect_bench() {
        REQUIRE(window->addServer(entry(), token).ok());
        REQUIRE(pump_until([&] {
            window->refresh();
            if (!window->selectServer("bench")) return false;
            window->refresh();
            return window->deviceRowCount() == 2;
        }));
    }
};

}  // namespace

TEST_CASE("client window adds a server, stores its token encrypted and lists devices") {
    Fixture f;
    f.connect_bench();
    CHECK(f.window->serverRowCount() == 1);
    CHECK(f.window->deviceRowCount() == 2);
    // the token is in the secret store, not in the configuration file
    REQUIRE(std::filesystem::exists(f.config_path));
    std::ifstream in(f.config_path);
    const std::string cfg_text((std::istreambuf_iterator<char>(in)), {});
    CHECK(cfg_text.find("bench") != std::string::npos);
    CHECK(cfg_text.find(f.token.substr(12)) == std::string::npos);
    CHECK(f.secrets.get("server:bench").ok());
}

TEST_CASE("attaching the selected device through the window plugs it and shows a session") {
    Fixture f;
    f.connect_bench();
    REQUIRE(f.window->selectDevice(f.engine->devices("bench").front().id));
    f.window->attachSelected(AccessMode::Exclusive);
    REQUIRE(pump_until([&] { return f.window->sessionRowCount() == 1; }));
    CHECK(f.vhci->plugged_count() == 1);
    CHECK(f.window->lastMessage().contains(QStringLiteral("Attached")));
    // the device row now reports it is attached to this PC; detach returns it to shared
    f.window->detachSelected();
    CHECK(pump_until([&] { return f.window->sessionRowCount() == 0; }));
    CHECK(f.vhci->plugged_count() == 0);
}

TEST_CASE("read-only attach creates a session without a virtual device") {
    Fixture f;
    f.connect_bench();
    REQUIRE(f.window->selectDevice(f.engine->devices("bench").front().id));
    f.window->attachSelected(AccessMode::SharedReadOnly);
    REQUIRE(pump_until([&] { return f.window->sessionRowCount() == 1; }));
    CHECK(f.vhci->plugged_count() == 0);
}

TEST_CASE("removing a server through the window deletes its token and configuration entry") {
    Fixture f;
    f.connect_bench();
    CHECK(f.window->selectServer("bench"));
    REQUIRE(f.window->removeServer("bench").ok());
    CHECK(f.window->serverRowCount() == 0);
    CHECK_FALSE(f.secrets.get("server:bench").ok());
    CHECK(f.engine->servers().empty());
}

TEST_CASE("duplicate server names and empty hosts are refused") {
    Fixture f;
    f.connect_bench();
    CHECK(f.window->addServer(f.entry(), f.token).error().code == ErrorCode::AlreadyExists);
    ServerEntry blank;
    blank.name = "x";
    CHECK(f.window->addServer(blank, f.token).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("client window switches to Persian (right-to-left) and back") {
    Fixture f;
    f.window->setLanguage(gui::Lang::Fa);
    CHECK(f.window->layoutDirection() == Qt::RightToLeft);
    f.window->setLanguage(gui::Lang::En);
    CHECK(f.window->layoutDirection() == Qt::LeftToRight);
}

TEST_CASE("engine reports that the TEST driver double is in use") {
    Fixture f;
    CHECK(f.engine->vhci_info().is_test_double);
}
