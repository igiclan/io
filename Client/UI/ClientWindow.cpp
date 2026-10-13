#include "UI/ClientWindow.hpp"

#include <map>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>

#include "Core/ClientEngine.hpp"
#include "UI/ClientStrings.hpp"

namespace pf::gui {

namespace {

using nlohmann::json;

const char* link_state_key(pf::client::LinkState s) {
    switch (s) {
        case pf::client::LinkState::Disconnected: return "state_disconnected";
        case pf::client::LinkState::Connecting: return "state_connecting";
        case pf::client::LinkState::Authenticating: return "state_authenticating";
        case pf::client::LinkState::Connected: return "state_connected";
        case pf::client::LinkState::Reconnecting: return "state_reconnecting";
        case pf::client::LinkState::Failed: return "state_failed";
    }
    return "state_disconnected";
}

const char* device_state_key(DeviceShareState s) {
    switch (s) {
        case DeviceShareState::Available: return "status_available";
        case DeviceShareState::Shared: return "status_shared";
        case DeviceShareState::InUse: return "status_in_use";
        case DeviceShareState::Unavailable: return "status_unavailable";
    }
    return "status_available";
}

void set_cell(QTableWidget* table, int row, int col, const QString& text) {
    auto* item = table->item(row, col);
    if (!item) {
        item = new QTableWidgetItem();
        table->setItem(row, col, item);
    }
    item->setText(text);
}

// Replaces a table's rows, keeping the selection on the row whose key matches.
int find_row_by_key(QTableWidget* table, const QString& key) {
    for (int r = 0; r < table->rowCount(); ++r) {
        const auto* item = table->item(r, 0);
        if (item && item->data(Qt::UserRole).toString() == key) return r;
    }
    return -1;
}

}  // namespace

ClientWindow::ClientWindow(pf::client::ClientEngine& engine, pf::SecretStore& secrets,
                           std::filesystem::path config_path, bool test_driver, QWidget* parent)
    : QMainWindow(parent),
      engine_(engine),
      secrets_(secrets),
      config_path_(std::move(config_path)),
      test_driver_(test_driver),
      bridge_(std::make_shared<UiBridge>()) {
    buildUi();
    setupTray();
    connect(bridge_.get(), &UiBridge::result, this, &ClientWindow::showResult);
    setLanguage(Lang::En);
    setDarkTheme(true);
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &ClientWindow::refresh);
    timer_->start(1000);
    refresh();
}

ClientWindow::~ClientWindow() = default;

void ClientWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    banner_ = new QLabel(central);
    banner_->setStyleSheet(QStringLiteral("QLabel { background: #b3261e; color: white; padding: 4px; font-weight: bold; }"));
    banner_->setVisible(test_driver_);
    root->addWidget(banner_);

    auto* status = new QHBoxLayout();
    client_label_ = new QLabel(central);
    vhci_label_ = new QLabel(central);
    sessions_label_ = new QLabel(central);
    status->addWidget(client_label_);
    status->addSpacing(24);
    status->addWidget(vhci_label_);
    status->addSpacing(24);
    status->addWidget(sessions_label_);
    status->addStretch(1);
    root->addLayout(status);

    // ---- servers ----
    servers_ = new QTableWidget(0, 8, central);
    servers_->setSelectionBehavior(QAbstractItemView::SelectRows);
    servers_->setSelectionMode(QAbstractItemView::SingleSelection);
    servers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    servers_->verticalHeader()->setVisible(false);
    servers_->horizontalHeader()->setStretchLastSection(true);
    connect(servers_, &QTableWidget::itemSelectionChanged, this, &ClientWindow::refresh);
    root->addWidget(servers_, 1);

    auto* server_buttons = new QHBoxLayout();
    add_btn_ = new QPushButton(central);
    remove_btn_ = new QPushButton(central);
    connect_btn_ = new QPushButton(central);
    disconnect_btn_ = new QPushButton(central);
    connect(add_btn_, &QPushButton::clicked, this, &ClientWindow::addServerDialog);
    connect(remove_btn_, &QPushButton::clicked, this, &ClientWindow::removeSelectedServer);
    connect(connect_btn_, &QPushButton::clicked, this, [this] { connectSelected(true); });
    connect(disconnect_btn_, &QPushButton::clicked, this, [this] { connectSelected(false); });
    for (auto* b : {add_btn_, remove_btn_, connect_btn_, disconnect_btn_}) server_buttons->addWidget(b);
    server_buttons->addStretch(1);
    root->addLayout(server_buttons);

    // ---- devices and sessions of the selected server ----
    tabs_ = new QTabWidget(central);
    devices_ = new QTableWidget(0, 8, tabs_);
    devices_->setSelectionBehavior(QAbstractItemView::SelectRows);
    devices_->setSelectionMode(QAbstractItemView::SingleSelection);
    devices_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    devices_->verticalHeader()->setVisible(false);
    devices_->horizontalHeader()->setStretchLastSection(true);
    connect(devices_, &QTableWidget::itemSelectionChanged, this, &ClientWindow::updateButtons);
    tabs_->addTab(devices_, QString());

    sessions_table_ = new QTableWidget(0, 9, tabs_);
    sessions_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sessions_table_->verticalHeader()->setVisible(false);
    sessions_table_->horizontalHeader()->setStretchLastSection(true);
    tabs_->addTab(sessions_table_, QString());
    root->addWidget(tabs_, 2);

    auto* device_buttons = new QHBoxLayout();
    attach_btn_ = new QPushButton(central);
    attach_ro_btn_ = new QPushButton(central);
    detach_btn_ = new QPushButton(central);
    reset_btn_ = new QPushButton(central);
    diag_btn_ = new QPushButton(central);
    connect(attach_btn_, &QPushButton::clicked, this,
            [this] { attachSelected(pf::AccessMode::Exclusive); });
    connect(attach_ro_btn_, &QPushButton::clicked, this,
            [this] { attachSelected(pf::AccessMode::SharedReadOnly); });
    connect(detach_btn_, &QPushButton::clicked, this, &ClientWindow::detachSelected);
    connect(reset_btn_, &QPushButton::clicked, this, &ClientWindow::resetSelected);
    connect(diag_btn_, &QPushButton::clicked, this, [this] {
        auto* dlg = new QPlainTextEdit(this);
        dlg->setReadOnly(true);
        dlg->setWindowTitle(ctl(lang_, "diag_title"));
        dlg->setPlainText(QString::fromStdString(engine_.metrics_json().dump(2)));
        dlg->resize(720, 520);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });
    for (auto* b : {attach_btn_, attach_ro_btn_, detach_btn_, reset_btn_, diag_btn_}) device_buttons->addWidget(b);
    device_buttons->addStretch(1);
    root->addLayout(device_buttons);

    message_ = new QLabel(central);
    message_->setWordWrap(true);
    root->addWidget(message_);

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
    resize(1200, 760);
}

void ClientWindow::setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    tray_ = new QSystemTrayIcon(style()->standardIcon(QStyle::SP_DesktopIcon), this);
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

void ClientWindow::setLanguage(Lang lang) {
    lang_ = lang;
    setLayoutDirection(lang == Lang::Fa ? Qt::RightToLeft : Qt::LeftToRight);
    applyTexts();
}

void ClientWindow::applyTexts() {
    const Lang l = lang_;
    setWindowTitle(ctl(l, "title"));
    banner_->setText(ctl(l, "test_driver"));
    if (tray_) tray_->setToolTip(ctl(l, "title"));
    if (tray_show_) tray_show_->setText(ctl(l, "tray_show"));
    if (tray_quit_) tray_quit_->setText(ctl(l, "tray_quit"));
    lang_label_->setText(ctl(l, "language") + QStringLiteral(":"));
    theme_label_->setText(ctl(l, "theme") + QStringLiteral(":"));
    add_btn_->setText(ctl(l, "btn_add_server"));
    remove_btn_->setText(ctl(l, "btn_remove_server"));
    connect_btn_->setText(ctl(l, "btn_connect"));
    disconnect_btn_->setText(ctl(l, "btn_disconnect"));
    attach_btn_->setText(ctl(l, "btn_attach"));
    attach_ro_btn_->setText(ctl(l, "btn_attach_ro"));
    detach_btn_->setText(ctl(l, "btn_detach"));
    reset_btn_->setText(ctl(l, "btn_reset"));
    diag_btn_->setText(ctl(l, "btn_diag"));
    tabs_->setTabText(0, ctl(l, "devices"));
    tabs_->setTabText(1, ctl(l, "sessions"));

    servers_->setHorizontalHeaderLabels({ctl(l, "col_server"), ctl(l, "col_address"), ctl(l, "col_state"),
                                         ctl(l, "col_latency"), ctl(l, "col_upload"), ctl(l, "col_download"),
                                         ctl(l, "col_reconnects"), ctl(l, "col_sessions")});
    devices_->setHorizontalHeaderLabels({ctl(l, "col_status"), ctl(l, "col_device"), ctl(l, "col_vid"),
                                         ctl(l, "col_pid"), ctl(l, "col_serial"), ctl(l, "col_speed"),
                                         ctl(l, "col_port"), ctl(l, "col_mode")});
    sessions_table_->setHorizontalHeaderLabels({ctl(l, "col_server"), ctl(l, "col_device"), ctl(l, "col_mode"),
                                                ctl(l, "col_state"), ctl(l, "col_access"), ctl(l, "col_handle"),
                                                ctl(l, "col_inflight"), ctl(l, "col_held"), ctl(l, "col_traffic")});

    const bool dark = dark_;
    theme_box_->blockSignals(true);
    theme_box_->clear();
    theme_box_->addItem(ctl(l, "theme_dark"), true);
    theme_box_->addItem(ctl(l, "theme_light"), false);
    theme_box_->setCurrentIndex(dark ? 0 : 1);
    theme_box_->blockSignals(false);
    lang_box_->blockSignals(true);
    lang_box_->setCurrentIndex(l == Lang::Fa ? 1 : 0);
    lang_box_->blockSignals(false);
    refresh();
}

void ClientWindow::setDarkTheme(bool dark) {
    dark_ = dark;
    apply_theme(dark);
}

void ClientWindow::refresh() {
    if (refreshing_) return;
    struct Guard {
        bool& flag;
        explicit Guard(bool& f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
    } guard{refreshing_};

    const json st = engine_.status_json();
    const json& vhci = st["virtual_usb"];
    client_label_->setText(ctl(lang_, "client_name") + QStringLiteral(": ") +
                           QString::fromStdString(st.value("client_name", "")));
    vhci_label_->setText(vhci.value("available", false) ? ctl(lang_, "vhci_ok") : ctl(lang_, "vhci_missing"));
    vhci_label_->setToolTip(QString::fromStdString(vhci.value("error", "")));
    sessions_label_->setText(ctl(lang_, "sessions_count") + QStringLiteral(": ") +
                             QString::number(st.value("sessions", 0)));

    // ---- servers ----
    const std::string selected_server = selectedServer();
    const auto servers = engine_.servers();
    servers_->setRowCount(static_cast<int>(servers.size()));
    for (int r = 0; r < static_cast<int>(servers.size()); ++r) {
        const auto& s = servers[static_cast<std::size_t>(r)];
        const QString name = QString::fromStdString(s.name);
        const QStringList cells{name,
                                QStringLiteral("%1:%2").arg(QString::fromStdString(s.host)).arg(s.port),
                                ctl(lang_, link_state_key(s.state)),
                                format_latency_us(static_cast<double>(s.rtt_us)),
                                format_rate(s.tx_bps),
                                format_rate(s.rx_bps),
                                QString::number(s.reconnects),
                                QString::number(s.sessions)};
        for (int c = 0; c < cells.size(); ++c) {
            set_cell(servers_, r, c, cells[c]);
            if (c == 0) servers_->item(r, 0)->setData(Qt::UserRole, name);
        }
    }
    selectServer(selected_server);

    // ---- devices of the selected server ----
    const std::string server = selectedServer();
    const std::string selected_device = selectedDeviceId();
    const auto sessions = engine_.sessions();
    std::map<std::string, std::string> attached_here;  // device id -> session id (this server only)
    for (const auto& s : sessions) {
        if (s->server() == server) attached_here[s->device().id] = s->id();
    }
    const auto devices = server.empty() ? std::vector<DeviceInfo>{} : engine_.devices(server);
    devices_->setRowCount(static_cast<int>(devices.size()));
    for (int r = 0; r < static_cast<int>(devices.size()); ++r) {
        const DeviceInfo& d = devices[static_cast<std::size_t>(r)];
        const bool mine = attached_here.count(d.id) > 0;
        const QStringList cells{mine ? ctl(lang_, "status_attached") : ctl(lang_, device_state_key(d.state)),
                                QString::fromStdString(d.display_name()),
                                QStringLiteral("%1").arg(d.identity.vid, 4, 16, QLatin1Char('0')),
                                QStringLiteral("%1").arg(d.identity.pid, 4, 16, QLatin1Char('0')),
                                text_or_dash(d.identity.serial),
                                QString::fromStdString(std::string(to_string(d.identity.speed))),
                                QString::number(d.identity.port),
                                QString::fromStdString(d.mode.label)};
        for (int c = 0; c < cells.size(); ++c) {
            set_cell(devices_, r, c, cells[c]);
            if (c == 0) devices_->item(r, 0)->setData(Qt::UserRole, QString::fromStdString(d.id));
        }
    }
    selectDevice(selected_device);

    // ---- sessions ----
    const auto& session_list = sessions;
    sessions_table_->setRowCount(static_cast<int>(session_list.size()));
    for (int r = 0; r < static_cast<int>(session_list.size()); ++r) {
        const json j = session_list[static_cast<std::size_t>(r)]->to_json();
        const json& usb = j["usb"];
        const QStringList cells{QString::fromStdString(j.value("server", "")),
                                QString::fromStdString(j.value("device", "")),
                                QString::fromStdString(j.value("mode", "")),
                                QString::fromStdString(j.value("state", "")),
                                ctl(lang_, j.value("access", "exclusive") == "shared-readonly" ? "access_shared"
                                                                                                  : "access_exclusive"),
                                QString::number(j.value("vhci_handle", 0u)),
                                QString::number(j.value("inflight", 0)),
                                QString::number(j.value("held", 0)),
                                format_bytes(usb.value("bytes_in", 0.0) + usb.value("bytes_out", 0.0))};
        for (int c = 0; c < cells.size(); ++c) set_cell(sessions_table_, r, c, cells[c]);
    }

    if (tray_) {
        tray_->setToolTip(QStringLiteral("%1\n%2: %3\n%4")
                              .arg(ctl(lang_, "title"))
                              .arg(ctl(lang_, "sessions"))
                              .arg(session_list.size())
                              .arg(vhci.value("available", false) ? ctl(lang_, "vhci_ok") : ctl(lang_, "vhci_missing")));
    }
    updateButtons();
}

int ClientWindow::serverRowCount() const { return servers_->rowCount(); }

std::string ClientWindow::selectedServer() const {
    const auto rows = servers_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return {};
    const auto* item = servers_->item(rows.first().row(), 0);
    return item ? item->data(Qt::UserRole).toString().toStdString() : std::string();
}

bool ClientWindow::selectServer(const std::string& name) {
    const int row = name.empty() ? -1 : find_row_by_key(servers_, QString::fromStdString(name));
    if (row < 0) {
        servers_->clearSelection();
        return false;
    }
    servers_->selectRow(row);
    return true;
}

int ClientWindow::deviceRowCount() const { return devices_->rowCount(); }

std::string ClientWindow::selectedDeviceId() const {
    const auto rows = devices_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return {};
    const auto* item = devices_->item(rows.first().row(), 0);
    return item ? item->data(Qt::UserRole).toString().toStdString() : std::string();
}

bool ClientWindow::selectDevice(const std::string& device_id) {
    const int row = device_id.empty() ? -1 : find_row_by_key(devices_, QString::fromStdString(device_id));
    if (row < 0) {
        devices_->clearSelection();
        return false;
    }
    devices_->selectRow(row);
    return true;
}

int ClientWindow::sessionRowCount() const { return sessions_table_->rowCount(); }

std::string ClientWindow::sessionIdForSelectedDevice() const {
    const std::string server = selectedServer();
    const std::string device = selectedDeviceId();
    for (const auto& s : engine_.sessions()) {
        if (s->server() == server && s->device().id == device) return s->id();
    }
    return {};
}

void ClientWindow::updateButtons() {
    const bool server_selected = !selectedServer().empty();
    const bool device_selected = !selectedDeviceId().empty();
    const bool vhci = engine_.vhci_available();
    const std::string server = selectedServer();
    DeviceShareState state = DeviceShareState::Unavailable;
    for (const auto& d : (server.empty() ? std::vector<DeviceInfo>{} : engine_.devices(server))) {
        if (d.id == selectedDeviceId()) state = d.state;
    }
    const bool attached = !sessionIdForSelectedDevice().empty();
    const bool offered = state == DeviceShareState::Shared;

    remove_btn_->setEnabled(server_selected);
    connect_btn_->setEnabled(server_selected);
    disconnect_btn_->setEnabled(server_selected);
    attach_btn_->setEnabled(device_selected && offered && !attached && vhci);
    attach_ro_btn_->setEnabled(device_selected && offered && !attached);
    detach_btn_->setEnabled(attached);
    reset_btn_->setEnabled(attached);
}

void ClientWindow::showResult(const QString& text, bool ok) {
    last_message_ = text;
    message_->setText(text);
    message_->setStyleSheet(ok ? QString() : QStringLiteral("color: #e06666;"));
    refresh();
}

QString ClientWindow::lastMessage() const { return last_message_; }

void ClientWindow::reportError(const pf::Error& error) {
    showResult(QString::fromStdString(error.describe()), false);
    QMessageBox::warning(this, ctl(lang_, "error_title"), QString::fromStdString(error.describe()));
}

void ClientWindow::attachSelected(pf::AccessMode access) {
    const std::string server = selectedServer();
    const std::string device_id = selectedDeviceId();
    if (server.empty()) return showResult(ctl(lang_, "no_server"), false);
    if (device_id.empty()) return showResult(ctl(lang_, "no_device"), false);
    SessionPriority prio = SessionPriority::High;
    parse_priority(engine_.config().default_priority, prio);
    const QString device_name = [&] {
        for (const auto& d : engine_.devices(server))
            if (d.id == device_id) return QString::fromStdString(d.display_name());
        return QString::fromStdString(device_id);
    }();
    // Callback runs on a network thread: report through the bridge only.
    engine_.attach(server, device_id, access, prio,
                   [bridge = bridge_, lang = lang_, device_name](Result<std::string> r) {
                       if (r) {
                           bridge->result(ctl(lang, "attached_ok") + device_name, true);
                       } else {
                           bridge->result(QString::fromStdString(r.error().describe()), false);
                       }
                   });
    showResult(QStringLiteral("…"), true);
}

void ClientWindow::detachSelected() {
    const std::string id = sessionIdForSelectedDevice();
    if (id.empty()) return showResult(ctl(lang_, "no_session"), false);
    if (confirm_ && QMessageBox::question(this, ctl(lang_, "title"), ctl(lang_, "confirm_detach")) != QMessageBox::Yes)
        return;
    if (auto st = engine_.detach(id); !st) return reportError(st.error());
    showResult(ctl(lang_, "detached_ok"), true);
}

void ClientWindow::resetSelected() {
    const std::string id = sessionIdForSelectedDevice();
    if (id.empty()) return showResult(ctl(lang_, "no_session"), false);
    engine_.reset_device(id);
    showResult(QStringLiteral("USB reset requested"), true);
}

void ClientWindow::connectSelected(bool connect) {
    const std::string name = selectedServer();
    if (name.empty()) return showResult(ctl(lang_, "no_server"), false);
    const Status st = connect ? engine_.connect(name) : engine_.disconnect(name);
    if (!st) return reportError(st.error());
    refresh();
}

pf::Status ClientWindow::addServer(const pf::ServerEntry& entry, const std::string& token) {
    if (entry.name.empty() || entry.host.empty()) return {ErrorCode::InvalidArgument, "name and host are required"};
    for (const auto& s : engine_.servers())
        if (s.name == entry.name) return {ErrorCode::AlreadyExists, "a server with this name already exists"};
    pf::ServerEntry e = entry;
    e.token_key = "server:" + e.name;
    if (!token.empty()) {
        if (auto st = secrets_.put(e.token_key, token); !st) return st;
    }
    if (auto st = engine_.add_server(e); !st) return st;
    persistConfig();
    refresh();
    return {};
}

pf::Status ClientWindow::removeServer(const std::string& name) {
    for (const auto& s : engine_.sessions()) {
        if (s->server() == name) (void)engine_.detach(s->id());
    }
    const std::string key = engine_.server_entry(name).token_key;
    if (auto st = engine_.remove_server(name); !st) return st;
    if (!key.empty()) (void)secrets_.erase(key);
    persistConfig();
    refresh();
    return {};
}

void ClientWindow::persistConfig() {
    if (auto st = pf::save_config(config_path_, pf::to_json(engine_.config())); !st) reportError(st.error());
}

void ClientWindow::removeSelectedServer() {
    const std::string name = selectedServer();
    if (name.empty()) return showResult(ctl(lang_, "no_server"), false);
    if (confirm_ && QMessageBox::question(this, ctl(lang_, "title"), ctl(lang_, "confirm_remove")) != QMessageBox::Yes)
        return;
    if (auto st = removeServer(name); !st) reportError(st.error());
}

void ClientWindow::addServerDialog() {
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle(ctl(lang_, "dlg_add_title"));
    auto* form = new QFormLayout(dlg);
    auto* name = new QLineEdit(dlg);
    auto* host = new QLineEdit(dlg);
    auto* port = new QSpinBox(dlg);
    port->setRange(1, 65535);
    port->setValue(39000);
    auto* tls = new QCheckBox(dlg);
    tls->setChecked(true);
    auto* fp = new QLineEdit(dlg);
    auto* token = new QLineEdit(dlg);
    token->setEchoMode(QLineEdit::Password);
    auto* autoattach = new QLineEdit(dlg);
    form->addRow(ctl(lang_, "lbl_name"), name);
    form->addRow(ctl(lang_, "lbl_host"), host);
    form->addRow(ctl(lang_, "lbl_port"), port);
    form->addRow(ctl(lang_, "lbl_tls"), tls);
    form->addRow(ctl(lang_, "lbl_fingerprint"), fp);
    form->addRow(ctl(lang_, "lbl_token"), token);
    form->addRow(ctl(lang_, "lbl_auto"), autoattach);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(ctl(lang_, "ok"));
    buttons->button(QDialogButtonBox::Cancel)->setText(ctl(lang_, "cancel"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dlg, [this, dlg, name, host, port, tls, fp, token, autoattach] {
        if (name->text().trimmed().isEmpty() || host->text().trimmed().isEmpty()) {
            QMessageBox::warning(dlg, ctl(lang_, "error_title"), ctl(lang_, "name_required"));
            return;
        }
        pf::ServerEntry e;
        e.name = name->text().trimmed().toStdString();
        e.host = host->text().trimmed().toStdString();
        e.port = static_cast<std::uint16_t>(port->value());
        e.tls = tls->isChecked();
        e.fingerprint = fp->text().trimmed().toStdString();
        if (!autoattach->text().trimmed().isEmpty()) e.auto_attach.push_back(autoattach->text().trimmed().toStdString());
        if (auto st = addServer(e, token->text().toStdString()); !st) {
            QMessageBox::warning(dlg, ctl(lang_, "error_title"), QString::fromStdString(st.error().describe()));
            return;
        }
        dlg->accept();
    });
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

void ClientWindow::closeEvent(QCloseEvent* event) {
    // With a tray icon the window hides and the client keeps its sessions; Quit is in the tray menu.
    if (tray_ && tray_->isVisible()) {
        hide();
        event->ignore();
        return;
    }
    event->accept();
}

}  // namespace pf::gui
