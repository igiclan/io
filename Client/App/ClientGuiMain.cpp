// ParsFlash USB Client - graphical application.
//
//   ParsFlash_USB_Client.exe [--config FILE]
//
// Hosts the client engine in-process. Closing the window keeps the client running in the
// system tray; use the tray menu's Quit to stop it.
//
// On Windows the virtual USB controller is the ParsFlash driver (pfvhci). Elsewhere the
// TEST driver double is used and the window shows a banner: devices are not exposed.
#include <QApplication>
#include <QMessageBox>
#include <memory>
#include <string>

#include "Common/Config.hpp"
#include "Common/Platform.hpp"
#include "Core/ClientEngine.hpp"
#include "Crypto/SecretStore.hpp"
#include "Logging/Logger.hpp"
#include "UI/ClientWindow.hpp"
#include "VirtualUSB/TestVhci.hpp"
#include "VirtualUSB/VirtualHci.hpp"

namespace {

void error_box(const QString& text) {
    QMessageBox::critical(nullptr, QStringLiteral("ParsFlash USB Client"), text);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ParsFlash USB Client"));
    QApplication::setApplicationVersion(QStringLiteral(PF_VERSION_STRING));

    std::filesystem::path config_path = pf::app_data_dir("client") / "config.json";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--config") config_path = argv[i + 1];
    }

    auto loaded = pf::load_client_config(config_path);
    if (!loaded) {
        error_box(QStringLiteral("Configuration error: %1").arg(QString::fromStdString(loaded.error().describe())));
        return 1;
    }
    const pf::ClientConfig cfg = *loaded;

    pf::LogLevel level = pf::LogLevel::Info;
    pf::parse_log_level(cfg.log_level, level);
    pf::Logger::instance().set_level(level);
    pf::Logger::instance().add_sink(
        std::make_shared<pf::RotatingFileSink>(std::filesystem::path(cfg.log_dir) / "client.log"));

    pf::SecretStore secrets(config_path.parent_path() / "secrets.json");
    if (auto st = secrets.load(); !st) {
        error_box(QStringLiteral("Cannot read stored secrets: %1").arg(QString::fromStdString(st.error().describe())));
        return 1;
    }

    bool test_driver = false;
    std::unique_ptr<pf::client::IVirtualHci> vhci;
#ifdef _WIN32
    vhci = pf::client::make_windows_vhci(cfg.vhci);
#else
    vhci = std::make_unique<pf::client::TestVhci>();
    test_driver = true;
#endif

    auto token_for = [&secrets](const pf::ServerEntry& e) {
        auto r = secrets.get(e.token_key);
        return r ? *r : std::string{};
    };
    pf::client::ClientEngine engine(cfg, std::move(vhci), token_for);
    if (auto st = engine.start(); !st) {
        error_box(QStringLiteral("Cannot start the client: %1").arg(QString::fromStdString(st.error().describe())));
        pf::Logger::instance().flush();
        return 1;
    }

    pf::gui::ClientWindow window(engine, secrets, config_path, test_driver);
    window.setLanguage(cfg.language == "fa" ? pf::gui::Lang::Fa : pf::gui::Lang::En);
    window.setDarkTheme(cfg.theme != "light");
    window.show();
    const int rc = app.exec();

    engine.stop();
    pf::Logger::instance().flush();
    return rc;
}
