// Main window of ParsFlash USB Server. Presents the engine's state; all actions go
// through ServerEngine's public, thread-safe API.
#pragma once

#include <string>

#include <QMainWindow>

#include "Common/Error.hpp"
#include "UI/Strings.hpp"

class QAction;
class QComboBox;
class QCloseEvent;
class QLabel;
class QPushButton;
class QSystemTrayIcon;
class QTableWidget;
class QTabWidget;
class QTimer;

namespace pf::server {
class ServerEngine;
}

namespace pf::gui {

class ServerWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ServerWindow(pf::server::ServerEngine& engine, QWidget* parent = nullptr);
    ~ServerWindow() override;

    void setLanguage(Lang lang);
    void setDarkTheme(bool dark);
    [[nodiscard]] Lang language() const noexcept { return lang_; }

    // Re-reads the engine state and updates every view (also driven by a 1 s timer).
    void refresh();
    // Shares or stops sharing the selected device. Shown to the user on failure.
    void shareSelected(bool shared);

    // Selection helpers (also used by tests).
    [[nodiscard]] int deviceRowCount() const;
    bool selectDevice(const std::string& device_id);
    [[nodiscard]] std::string selectedDeviceId() const;
    [[nodiscard]] QString deviceRowStatus(int row) const;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void applyTexts();
    void setupTray();
    void updateButtons();
    void onReset();
    void onDetach();
    void onDiagnostics();
    void reportError(const pf::Error& error);

    pf::server::ServerEngine& engine_;
    Lang lang_ = Lang::En;
    bool dark_ = true;

    QLabel* online_ = nullptr;
    QLabel* address_ = nullptr;
    QLabel* fingerprint_ = nullptr;
    QLabel* clients_ = nullptr;
    QLabel* shared_ = nullptr;
    QLabel* sessions_ = nullptr;
    QLabel* upload_ = nullptr;
    QLabel* download_ = nullptr;
    QLabel* name_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTableWidget* devices_ = nullptr;
    QTableWidget* clients_table_ = nullptr;
    QTableWidget* sessions_table_ = nullptr;
    QPushButton* share_btn_ = nullptr;
    QPushButton* unshare_btn_ = nullptr;
    QPushButton* reset_btn_ = nullptr;
    QPushButton* detach_btn_ = nullptr;
    QPushButton* diag_btn_ = nullptr;
    QComboBox* lang_box_ = nullptr;
    QComboBox* theme_box_ = nullptr;
    QLabel* lang_label_ = nullptr;
    QLabel* theme_label_ = nullptr;
    QTimer* timer_ = nullptr;

    QSystemTrayIcon* tray_ = nullptr;
    QAction* tray_show_ = nullptr;
    QAction* tray_quit_ = nullptr;
};

}  // namespace pf::gui
