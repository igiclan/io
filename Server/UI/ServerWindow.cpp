#include "UI/ServerWindow.hpp"

#include <map>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QStyleFactory>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>

#include "Common/Platform.hpp"
#include "Core/ServerEngine.hpp"

namespace pf::gui {

namespace {

using nlohmann::json;

QString text_or_dash(const std::string& s) { return s.empty() ? QStringLiteral("-") : QString::fromStdString(s); }

QString format_rate(double bps) {
    if (bps >= 1e9) return QStringLiteral("%1 Gb/s").arg(bps / 1e9, 0, 'f', 2);
    if (bps >= 1e6) return QStringLiteral("%1 Mb/s").arg(bps / 1e6, 0, 'f', 2);
    if (bps >= 1e3) return QStringLiteral("%1 Kb/s").arg(bps / 1e3, 0, 'f', 1);
    return QStringLiteral("%1 b/s").arg(bps, 0, 'f', 0);
}

QString format_bytes(double b) {
    if (b >= 1e9) return QStringLiteral("%1 GB").arg(b / 1e9, 0, 'f', 2);
    if (b >= 1e6) return QStringLiteral("%1 MB").arg(b / 1e6, 0, 'f', 2);
    if (b >= 1e3) return QStringLiteral("%1 KB").arg(b / 1e3, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(b, 0, 'f', 0);
}

QString format_latency(double p50_us) {
    if (p50_us <= 0) return QStringLiteral("-");
    return QStringLiteral("%1 ms").arg(p50_us / 1000.0, 0, 'f', 2);
}

// Maps a device state string from the engine JSON to a translation key.
const char* status_key(DeviceShareState s) {
    switch (s) {
        case DeviceShareState::Available: return "status_available";
        case DeviceShareState::Shared: return "status_shared";
        case DeviceShareState::InUse: return "status_in_use";
        case DeviceShareState::Unavailable: return "status_unavailable";
    }
    return "status_available";
}

void apply_dark_palette() {
    QPalette p;
    const QColor window(30, 30, 34), base(22, 22, 26), alt(36, 36, 42), button(45, 45, 52), text(230, 230, 235),
        highlight(56, 132, 255);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::ToolTipBase, alt);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 130));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 130));
    qApp->setPalette(p);
}

}  // namespace

ServerWindow::ServerWindow(pf::server::ServerEngine& engine, QWidget* parent) : QMainWindow(parent), engine_(engine) {
    buildUi();
    setupTray();
    setLanguage(Lang::En);
    setDarkTheme(true);
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &ServerWindow::refresh);
    timer_->start(1000);
    refresh();
}

ServerWindow::~ServerWindow() = default;

void ServerWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    // ---- status panel ----
    auto* status = new QGroupBox(this);
    auto* grid = new QFormLayout(status);
    name_ = new QLabel(status);
    online_ = new QLabel(status);
    address_ = new QLabel(status);
    fingerprint_ = new QLabel(status);
    fingerprint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    clients_ = new QLabel(status);
    shared_ = new QLabel(status);
    sessions_ = new QLabel(status);
    upload_ = new QLabel(status);
    download_ = new QLabel(status);
    grid->addRow(name_, online_);
    grid->addRow(address_, fingerprint_);
    grid->addRow(clients_, shared_);
    grid->addRow(sessions_, upload_);
    grid->addRow(download_, new QLabel(status));
    root->addWidget(status);

    // ---- tabs ----
    tabs_ = new QTabWidget(central);
    devices_ = new QTableWidget(0, 11, tabs_);
    devices_->setSelectionBehavior(QAbstractItemView::SelectRows);
    devices_->setSelectionMode(QAbstractItemView::SingleSelection);
    devices_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    devices_->verticalHeader()->setVisible(false);
    devices_->horizontalHeader()->setStretchLastSection(true);
    connect(devices_, &QTableWidget::itemSelectionChanged, this, &ServerWindow::updateButtons);
    tabs_->addTab(devices_, QString());

    clients_table_ = new QTableWidget(0, 6, tabs_);
    clients_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    clients_table_->verticalHeader()->setVisible(false);
    clients_table_->horizontalHeader()->setStretchLastSection(true);
    tabs_->addTab(clients_table_, QString());

    sessions_table_ = new QTableWidget(0, 7, tabs_);
    sessions_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sessions_table_->verticalHeader()->setVisible(false);
    sessions_table_->horizontalHeader()->setStretchLastSection(true);
    tabs_->addTab(sessions_table_, QString());
    root->addWidget(tabs_, 1);

    // ---- actions ----
    auto* actions = new QHBoxLayout();
    share_btn_ = new QPushButton(central);
    unshare_btn_ = new QPushButton(central);
    reset_btn_ = new QPushButton(central);
    detach_btn_ = new QPushButton(central);
    diag_btn_ = new QPushButton(central);
    connect(share_btn_, &QPushButton::clicked, this, [this] { shareSelected(true); });
    connect(unshare_btn_, &QPushButton::clicked, this, [this] { shareSelected(false); });
    connect(reset_btn_, &QPushButton::clicked, this, &ServerWindow::onReset);
    connect(detach_btn_, &QPushButton::clicked, this, &ServerWindow::onDetach);
    connect(diag_btn_, &QPushButton::clicked, this, &ServerWindow::onDiagnostics);
    for (auto* b : {share_btn_, unshare_btn_, reset_btn_, detach_btn_, diag_btn_}) actions->addWidget(b);
    actions->addStretch(1);
    root->addLayout(actions);

    // ---- language and theme ----
    auto* prefs = new QHBoxLayout();
    lang_label_ = new QLabel(central);
    lang_box_ = new QComboBox(central);
    lang_box_->addItem(QStringLiteral("English"), static_cast<int>(Lang::En));
    lang_box_->addItem(QStringLiteral("فارسی"), static_cast<int>(Lang::Fa));
    theme_label_ = new QLabel(central);
    theme_box_ = new QComboBox(central);
    connect(lang_box_, &QComboBox::currentIndexChanged, this, [this](int i) {
        setLanguage(static_cast<Lang>(lang_box_->itemData(i).toInt()));
    });
    connect(theme_box_, &QComboBox::currentIndexChanged, this, [this](int) {
        setDarkTheme(theme_box_->currentData().toBool());
    });
    prefs->addWidget(lang_label_);
    prefs->addWidget(lang_box_);
    prefs->addSpacing(24);
    prefs->addWidget(theme_label_);
    prefs->addWidget(theme_box_);
    prefs->addStretch(1);
    root->addLayout(prefs);

    setCentralWidget(central);
    resize(1100, 700);
}

void ServerWindow::setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    tray_ = new QSystemTrayIcon(style()->standardIcon(QStyle::SP_ComputerIcon), this);
    auto* menu = new QMenu(this);
    tray_show_ = menu->addAction(QString(), this, [this] {
        show();
        raise();
        activateWindow();
    });
    menu->addSeparator();
    tray_quit_ = menu->addAction(QString(), qApp, &QApplication::quit);
    tray_->setContextMenu(menu);
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r != QSystemTrayIcon::Trigger) return;
        if (isVisible()) {
            hide();
        } else {
            show();
            raise();
        }
    });
    tray_->show();
}

void ServerWindow::setLanguage(Lang lang) {
    lang_ = lang;
    setLayoutDirection(lang == Lang::Fa ? Qt::RightToLeft : Qt::LeftToRight);
    applyTexts();
}

void ServerWindow::applyTexts() {
    const Lang l = lang_;
    setWindowTitle(tl(l, "title"));
    if (tray_) tray_->setToolTip(tl(l, "title"));
    if (tray_show_) tray_show_->setText(tl(l, "tray_show"));
    if (tray_quit_) tray_quit_->setText(tl(l, "tray_quit"));
    lang_label_->setText(tl(l, "language") + QStringLiteral(":"));
    theme_label_->setText(tl(l, "theme") + QStringLiteral(":"));
    // Labels keep their current values; only the captions change.
    tabs_->setTabText(0, tl(l, "tab_devices"));
    tabs_->setTabText(1, tl(l, "tab_clients"));
    tabs_->setTabText(2, tl(l, "tab_sessions"));
    share_btn_->setText(tl(l, "btn_share"));
    unshare_btn_->setText(tl(l, "btn_unshare"));
    reset_btn_->setText(tl(l, "btn_reset"));
    detach_btn_->setText(tl(l, "btn_detach"));
    diag_btn_->setText(tl(l, "btn_diag"));

    devices_->setHorizontalHeaderLabels({tl(l, "col_status"), tl(l, "col_device"), tl(l, "col_vid"), tl(l, "col_pid"),
                                         tl(l, "col_serial"), tl(l, "col_speed"), tl(l, "col_port"), tl(l, "col_mode"),
                                         tl(l, "col_client"), tl(l, "col_traffic"), tl(l, "col_latency")});
    clients_table_->setHorizontalHeaderLabels({tl(l, "col_client"), tl(l, "col_address"), tl(l, "col_secure"),
                                               tl(l, "col_speed"), tl(l, "col_latency"), tl(l, "col_traffic")});
    sessions_table_->setHorizontalHeaderLabels({tl(l, "col_client"), tl(l, "col_device"), tl(l, "col_mode"),
                                                tl(l, "col_state"), tl(l, "col_access"), tl(l, "col_inflight"),
                                                tl(l, "col_traffic")});

    // theme and language choosers: keep the selection, refresh the captions
    const bool dark = dark_;
    theme_box_->blockSignals(true);
    theme_box_->clear();
    theme_box_->addItem(tl(l, "theme_dark"), true);
    theme_box_->addItem(tl(l, "theme_light"), false);
    theme_box_->setCurrentIndex(dark ? 0 : 1);
    theme_box_->blockSignals(false);
    lang_box_->blockSignals(true);
    lang_box_->setCurrentIndex(l == Lang::Fa ? 1 : 0);
    lang_box_->blockSignals(false);
    refresh();
}

void ServerWindow::setDarkTheme(bool dark) {
    dark_ = dark;
    if (dark) {
        qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        apply_dark_palette();
    } else {
        qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        qApp->setPalette(qApp->style()->standardPalette());
    }
}

void ServerWindow::refresh() {
    const json st = engine_.status_json();
    const bool online = st.value("online", false);
    name_->setText(tl(lang_, "server_name") + QStringLiteral(": ") + QString::fromStdString(st.value("server_name", "")));
    online_->setText(online ? tl(lang_, "online") : tl(lang_, "offline"));
    address_->setText(tl(lang_, "address") + QStringLiteral(": ") + QStringLiteral("%1:%2")
                                                                  .arg(QString::fromStdString(st.value("listen_address", "")))
                                                                  .arg(st.value("port", 0)));
    const std::string fp = st.value("fingerprint", "");
    fingerprint_->setText(tl(lang_, "fingerprint") + QStringLiteral(": ") +
                          (st.value("tls", true) ? text_or_dash(fp) : tl(lang_, "tls_off")));
    clients_->setText(tl(lang_, "clients") + QStringLiteral(": ") + QString::number(st.value("clients", 0)));
    shared_->setText(tl(lang_, "shared_devices") + QStringLiteral(": ") + QString::number(st.value("shared_devices", 0)));
    sessions_->setText(tl(lang_, "sessions") + QStringLiteral(": ") + QString::number(st.value("sessions", 0)));
    upload_->setText(tl(lang_, "upload") + QStringLiteral(": ") + format_rate(st.value("upload_bps", 0.0)));
    download_->setText(tl(lang_, "download") + QStringLiteral(": ") + format_rate(st.value("download_bps", 0.0)));

    // per-session USB metrics, keyed by device id, for the traffic and latency columns
    std::map<std::string, json> by_device;
    for (const auto& s : engine_.sessions_json()) by_device[s.value("device_id", "")] = s;

    const std::string selected = selectedDeviceId();
    const auto devices = engine_.devices();
    devices_->setRowCount(static_cast<int>(devices.size()));
    for (int r = 0; r < static_cast<int>(devices.size()); ++r) {
        const DeviceInfo& d = devices[static_cast<std::size_t>(r)];
        QString traffic = QStringLiteral("-"), latency = QStringLiteral("-");
        if (auto it = by_device.find(d.id); it != by_device.end()) {
            const json& usb = it->second["usb"];
            traffic = format_bytes(usb.value("bytes_in", 0.0) + usb.value("bytes_out", 0.0));
            latency = format_latency(usb["latency"].value("p50_us", 0.0));
        }
        const QStringList cells{tl(lang_, status_key(d.state)),
                                QString::fromStdString(d.display_name()),
                                QStringLiteral("%1").arg(d.identity.vid, 4, 16, QLatin1Char('0')),
                                QStringLiteral("%1").arg(d.identity.pid, 4, 16, QLatin1Char('0')),
                                text_or_dash(d.identity.serial),
                                QString::fromStdString(std::string(to_string(d.identity.speed))),
                                QString::number(d.identity.port),
                                QString::fromStdString(d.mode.label),
                                text_or_dash(d.owner_name),
                                traffic,
                                latency};
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = devices_->item(r, c);
            if (!item) {
                item = new QTableWidgetItem();
                devices_->setItem(r, c, item);
            }
            item->setText(cells[c]);
            if (c == 0) item->setData(Qt::UserRole, QString::fromStdString(d.id));
        }
    }
    selectDevice(selected);

    const auto clients = engine_.clients();
    clients_table_->setRowCount(static_cast<int>(clients.size()));
    for (int r = 0; r < static_cast<int>(clients.size()); ++r) {
        const auto& c = clients[static_cast<std::size_t>(r)];
        const QStringList cells{QString::fromStdString(c.name), QString::fromStdString(c.address),
                                c.secure ? tl(lang_, "yes") : tl(lang_, "no"),
                                format_rate(c.tx_bps + c.rx_bps), format_latency(static_cast<double>(c.rtt_us)),
                                QString::number(c.sessions)};
        for (int k = 0; k < cells.size(); ++k) {
            auto* item = clients_table_->item(r, k);
            if (!item) {
                item = new QTableWidgetItem();
                clients_table_->setItem(r, k, item);
            }
            item->setText(cells[k]);
        }
    }

    const json sessions = engine_.sessions_json();
    sessions_table_->setRowCount(static_cast<int>(sessions.size()));
    for (int r = 0; r < static_cast<int>(sessions.size()); ++r) {
        const json& s = sessions[static_cast<std::size_t>(r)];
        const json& usb = s["usb"];
        const QStringList cells{QString::fromStdString(s.value("client_name", "")),
                                QString::fromStdString(s.value("device", "")),
                                QString::fromStdString(s.value("mode", "")),
                                QString::fromStdString(s.value("state", "")),
                                QString::fromStdString(s.value("access", "")),
                                QString::number(s.value("inflight", 0)),
                                format_bytes(usb.value("bytes_in", 0.0) + usb.value("bytes_out", 0.0))};
        for (int k = 0; k < cells.size(); ++k) {
            auto* item = sessions_table_->item(r, k);
            if (!item) {
                item = new QTableWidgetItem();
                sessions_table_->setItem(r, k, item);
            }
            item->setText(cells[k]);
        }
    }

    if (tray_) {
        tray_->setToolTip(QStringLiteral("%1\n%2: %3\n%4: %5\n%6")
                              .arg(tl(lang_, "title"))
                              .arg(tl(lang_, "shared_devices"))
                              .arg(st.value("shared_devices", 0))
                              .arg(tl(lang_, "clients"))
                              .arg(st.value("clients", 0))
                              .arg(online ? tl(lang_, "online") : tl(lang_, "offline")));
    }
    updateButtons();
}

int ServerWindow::deviceRowCount() const { return devices_->rowCount(); }

std::string ServerWindow::selectedDeviceId() const {
    const auto rows = devices_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return {};
    const auto* item = devices_->item(rows.first().row(), 0);
    return item ? item->data(Qt::UserRole).toString().toStdString() : std::string();
}

QString ServerWindow::deviceRowStatus(int row) const {
    const auto* item = devices_->item(row, 0);
    return item ? item->text() : QString();
}

bool ServerWindow::selectDevice(const std::string& device_id) {
    if (device_id.empty()) {
        devices_->clearSelection();
        return false;
    }
    for (int r = 0; r < devices_->rowCount(); ++r) {
        const auto* item = devices_->item(r, 0);
        if (item && item->data(Qt::UserRole).toString().toStdString() == device_id) {
            devices_->selectRow(r);
            return true;
        }
    }
    devices_->clearSelection();
    return false;
}

void ServerWindow::updateButtons() {
    const std::string id = selectedDeviceId();
    const DeviceInfo* info = nullptr;
    const auto devices = engine_.devices();
    for (const auto& d : devices)
        if (d.id == id) info = &d;
    const bool has = info != nullptr;
    share_btn_->setEnabled(has && !info->shared && info->state != DeviceShareState::Unavailable);
    unshare_btn_->setEnabled(has && info->shared && info->state != DeviceShareState::InUse);
    reset_btn_->setEnabled(has);
    detach_btn_->setEnabled(has && info->state == DeviceShareState::InUse);
}

void ServerWindow::reportError(const pf::Error& error) {
    QMessageBox::warning(this, tl(lang_, "error_title"), QString::fromStdString(error.describe()));
}

void ServerWindow::shareSelected(bool shared) {
    const std::string id = selectedDeviceId();
    if (id.empty()) {
        QMessageBox::information(this, tl(lang_, "title"), tl(lang_, "no_selection"));
        return;
    }
    if (auto st = engine_.share(id, shared); !st) reportError(st.error());
    refresh();
}

void ServerWindow::onReset() {
    const std::string id = selectedDeviceId();
    if (id.empty()) return;
    if (auto st = engine_.reset_device(id); !st) reportError(st.error());
    refresh();
}

void ServerWindow::onDetach() {
    const std::string id = selectedDeviceId();
    if (id.empty()) return;
    const auto answer = QMessageBox::question(this, tl(lang_, "title"), tl(lang_, "confirm_detach"));
    if (answer != QMessageBox::Yes) return;
    if (auto st = engine_.detach_device(id, "detached from the server console"); !st) reportError(st.error());
    refresh();
}

void ServerWindow::onDiagnostics() {
    auto* dlg = new QPlainTextEdit(this);
    dlg->setReadOnly(true);
    dlg->setWindowTitle(tl(lang_, "diag_title"));
    dlg->setPlainText(QString::fromStdString(engine_.metrics_json().dump(2)));
    dlg->resize(720, 520);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void ServerWindow::closeEvent(QCloseEvent* event) {
    // With a tray icon the window hides and the server keeps running; Quit is in the tray menu.
    if (tray_ && tray_->isVisible()) {
        hide();
        event->ignore();
        return;
    }
    event->accept();
}

}  // namespace pf::gui
