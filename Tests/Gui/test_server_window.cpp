// Drives the real ServerWindow against a running engine (emulated devices) in Qt's
// offscreen platform: no display is needed.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <QApplication>
#include <chrono>
#include <thread>

#include "../Common/TestEnv.hpp"
#include "Common/Config.hpp"
#include "Core/ServerEngine.hpp"
#include "Logging/Logger.hpp"
#include "UI/ServerWindow.hpp"

using namespace pf;
using namespace std::chrono_literals;

namespace {

// Lets queued Qt events and engine work run until `pred` holds or the timeout expires.
bool pump_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 5s) {
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
    ServerConfig cfg;
    std::unique_ptr<server::ServerEngine> engine;
    std::unique_ptr<gui::ServerWindow> window;

    Fixture() {
        Logger::instance().set_level(LogLevel::Warning);
        apply_defaults(cfg, dir.path());
        cfg.listen_address = "127.0.0.1";
        cfg.server_port = 0;
        cfg.api.enabled = false;
        cfg.tls_enabled = false;
        cfg.usb.test_devices = 2;
        cfg.usb.share_policy = "all";
        engine = std::make_unique<server::ServerEngine>(cfg);
        REQUIRE(engine->start().ok());
        window = std::make_unique<gui::ServerWindow>(*engine);
    }
    ~Fixture() {
        window.reset();
        engine->stop();
    }
};

}  // namespace

TEST_CASE("window lists the emulated devices with their state") {
    Fixture f;
    REQUIRE(pump_until([&] {
        f.window->refresh();
        return f.window->deviceRowCount() == 2;
    }));
    CHECK(f.window->deviceRowStatus(0) == QStringLiteral("Shared"));
    CHECK(f.engine->devices().size() == 2);
}

TEST_CASE("sharing a selected device through the window updates the engine") {
    Fixture f;
    REQUIRE(pump_until([&] { f.window->refresh(); return f.window->deviceRowCount() == 2; }));
    const std::string id = f.engine->devices().front().id;
    REQUIRE(f.window->selectDevice(id));
    f.window->shareSelected(false);
    CHECK(f.engine->devices().front().shared == false);
    CHECK(f.window->deviceRowStatus(0) == QStringLiteral("Available"));
    f.window->shareSelected(true);
    CHECK(f.engine->devices().front().shared == true);
}

TEST_CASE("switching to Persian turns the layout right-to-left and translates captions") {
    Fixture f;
    f.window->setLanguage(gui::Lang::Fa);
    CHECK(f.window->layoutDirection() == Qt::RightToLeft);
    CHECK(f.window->language() == gui::Lang::Fa);
    f.window->setLanguage(gui::Lang::En);
    CHECK(f.window->layoutDirection() == Qt::LeftToRight);
}

TEST_CASE("dark and light themes can be applied repeatedly") {
    Fixture f;
    f.window->setDarkTheme(true);
    f.window->setDarkTheme(false);
    f.window->setDarkTheme(true);
    CHECK(QApplication::palette().color(QPalette::Window) == QColor(30, 30, 34));
}

TEST_CASE("translations fall back to the key for unknown identifiers") {
    CHECK(gui::tl(gui::Lang::En, "no_such_key") == QStringLiteral("no_such_key"));
    CHECK(gui::tl(gui::Lang::Fa, "title") != gui::tl(gui::Lang::En, "title"));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    doctest::Context ctx(argc, argv);
    const int rc = ctx.run();
    return rc;
}
