// ParsFlash USB Server - graphical application.
//
//   ParsFlash_USB_Server.exe [--config FILE]
//
// Hosts the server engine in-process. Closing the window keeps the server running in the
// system tray; use the tray menu's Quit to stop it.
#include <QApplication>
#include <QMessageBox>
#include <memory>
#include <string>

#include "Common/Config.hpp"
#include "Common/Platform.hpp"
#include "Core/ServerEngine.hpp"
#include "Logging/Logger.hpp"
#include "UI/ServerWindow.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ParsFlash USB Server"));
    QApplication::setApplicationVersion(QStringLiteral(PF_VERSION_STRING));

    std::filesystem::path config_path = pf::app_data_dir("server") / "config.json";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--config") config_path = argv[i + 1];
    }

    auto loaded = pf::load_server_config(config_path);
    if (!loaded) {
        QMessageBox::critical(nullptr, QStringLiteral("ParsFlash USB Server"),
                              QStringLiteral("Configuration error: %1").arg(QString::fromStdString(loaded.error().describe())));
        return 1;
    }
    const pf::ServerConfig cfg = *loaded;

    pf::LogLevel level = pf::LogLevel::Info;
    pf::parse_log_level(cfg.log_level, level);
    pf::Logger::instance().set_level(level);
    pf::Logger::instance().add_sink(
        std::make_shared<pf::RotatingFileSink>(std::filesystem::path(cfg.log_dir) / "server.log"));

    pf::server::ServerEngine engine(cfg);
    if (auto st = engine.start(); !st) {
        QMessageBox::critical(nullptr, QStringLiteral("ParsFlash USB Server"),
                              QStringLiteral("Cannot start the server: %1").arg(QString::fromStdString(st.error().describe())));
        pf::Logger::instance().flush();
        return 1;
    }

    pf::gui::ServerWindow window(engine);
    window.show();
    const int rc = app.exec();

    engine.stop();
    pf::Logger::instance().flush();
    return rc;
}
