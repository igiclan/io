// Main window of ParsFlash USB Client. Presents the client engine's state and turns user
// actions into engine calls. Engine callbacks arrive on network threads and reach the
// window through a queued signal.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <QMainWindow>
#include <QObject>

#include "Common/Config.hpp"
#include "Crypto/SecretStore.hpp"
#include "Kit/UiKit.hpp"
#include "Models/UsbTypes.hpp"

class QAction;
class QCloseEvent;
class QComboBox;
class QLabel;
class QPushButton;
class QSystemTrayIcon;
class QTableWidget;
class QTabWidget;
class QTimer;

namespace pf::client {
class ClientEngine;
}

namespace pf::gui {

// Carries engine results from network threads to the GUI thread (queued connection).
class UiBridge : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    void result(QString text, bool ok);
};

class ClientWindow : public QMainWindow {
    Q_OBJECT
public:
    // `config_path` is where server changes are saved; `secrets` holds server tokens.
    ClientWindow(pf::client::ClientEngine& engine, pf::SecretStore& secrets, std::filesystem::path config_path,
                 bool test_driver, QWidget* parent = nullptr);
    ~ClientWindow() override;

    void setLanguage(Lang lang);
    void setDarkTheme(bool dark);
    [[nodiscard]] Lang language() const noexcept { return lang_; }

    void refresh();

    // Detach and remove ask for confirmation by default. Automated tests turn this off
    // because a modal dialog would block them.
    void setConfirmations(bool enabled) { confirm_ = enabled; }

    // Server list management. The token is stored encrypted, never in the config file.
    pf::Status addServer(const pf::ServerEntry& entry, const std::string& token);
    pf::Status removeServer(const std::string& name);

    // Selection helpers (also used by tests).
    [[nodiscard]] int serverRowCount() const;
    bool selectServer(const std::string& name);
    [[nodiscard]] std::string selectedServer() const;
    [[nodiscard]] int deviceRowCount() const;
    bool selectDevice(const std::string& device_id);
    [[nodiscard]] std::string selectedDeviceId() const;
    [[nodiscard]] int sessionRowCount() const;

    // Attaches the selected device. The result arrives asynchronously via resultMessage().
    void attachSelected(pf::AccessMode access);
    void detachSelected();
    void resetSelected();

    // Last message shown in the status line (tests read it).
    [[nodiscard]] QString lastMessage() const;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void applyTexts();
    void setupTray();
    void updateButtons();
    void showResult(const QString& text, bool ok);
    void reportError(const pf::Error& error);
    void persistConfig();
    void addServerDialog();
    void removeSelectedServer();
    void connectSelected(bool connect);
    std::string sessionIdForSelectedDevice() const;

    pf::client::ClientEngine& engine_;
    pf::SecretStore& secrets_;
    std::filesystem::path config_path_;
    Lang lang_ = Lang::En;
    bool dark_ = true;
    bool test_driver_ = false;
    bool confirm_ = true;
    bool refreshing_ = false;  // guards against refresh() re-entering through selection changes
    QString last_message_;

    QLabel* banner_ = nullptr;
    QLabel* client_label_ = nullptr;
    QLabel* vhci_label_ = nullptr;
    QLabel* sessions_label_ = nullptr;
    QTableWidget* servers_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTableWidget* devices_ = nullptr;
    QTableWidget* sessions_table_ = nullptr;
    QLabel* message_ = nullptr;
    QPushButton* add_btn_ = nullptr;
    QPushButton* remove_btn_ = nullptr;
    QPushButton* connect_btn_ = nullptr;
    QPushButton* disconnect_btn_ = nullptr;
    QPushButton* attach_btn_ = nullptr;
    QPushButton* attach_ro_btn_ = nullptr;
    QPushButton* detach_btn_ = nullptr;
    QPushButton* reset_btn_ = nullptr;
    QPushButton* diag_btn_ = nullptr;
    QComboBox* lang_box_ = nullptr;
    QComboBox* theme_box_ = nullptr;
    QLabel* lang_label_ = nullptr;
    QLabel* theme_label_ = nullptr;
    QTimer* timer_ = nullptr;

    QSystemTrayIcon* tray_ = nullptr;
    QAction* tray_show_ = nullptr;
    QAction* tray_quit_ = nullptr;

    std::shared_ptr<UiBridge> bridge_;
};

}  // namespace pf::gui
