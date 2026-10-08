#include "Core/ServerEngine.hpp"

#include <algorithm>
#include <unordered_map>

#include "Crypto/Crypto.hpp"
#include "Logging/Logger.hpp"
#include "USB/LoopbackBackend.hpp"

namespace pf::server {

using namespace proto;
using nlohmann::json;

namespace {
constexpr int kMaxAuthFailures = 3;

std::string short_id(const std::string& s) { return s.size() > 8 ? s.substr(0, 8) : s; }
}  // namespace

// =====================================================================
// ClientConn: one TCP/TLS connection from a ParsFlash USB Client.
// All handlers run on the connection strand.
// =====================================================================
class ClientConn : public std::enable_shared_from_this<ClientConn> {
public:
    ClientConn(ServerEngine& e, std::string id, std::shared_ptr<net::FrameConnection> c)
        : eng_(e), id_(std::move(id)), conn_(std::move(c)), auth_timer_(conn_->strand()),
          connected_ms_(unix_time_ms()) {}

    void start() {
        auto self = shared_from_this();
        conn_->start({[self](InFrame&& f) { self->on_frame(std::move(f)); },
                      [self](const Error& e) { self->on_closed(e); }, nullptr});
        auth_timer_.expires_after(std::chrono::milliseconds(eng_.cfg_.auth_timeout_ms));
        auth_timer_.async_wait([self](std::error_code ec) {
            if (!ec && !self->authed_) {
                LogRecord(LogLevel::Warning, "Security").client(self->conn_->remote_address()).op("auth").result(
                    "timeout");
                self->fail(ErrorCode::AuthRequired, "authentication timeout");
            }
        });
    }

    void close(const std::string& reason) {
        conn_->send(encode_json(MsgType::Goodbye, json{{"reason", reason}}), true);
        conn_->close_after_flush(Error{ErrorCode::Ok, reason});
    }

    ClientSnapshot snapshot() const {
        std::lock_guard lk(info_m_);
        ClientSnapshot s;
        s.connection_id = id_;
        s.client_id = client_id_;
        s.name = client_name_;
        s.address = conn_->remote_address();
        s.token_name = token_.name;
        s.authenticated = authed_;
        s.secure = conn_->is_secure();
        s.connected_ms = connected_ms_;
        s.rtt_us = conn_->last_rtt_us();
        s.tx_bps = conn_->metrics()->tx.rate() * 8;
        s.rx_bps = conn_->metrics()->rx.rate() * 8;
        s.sessions = session_count_;
        return s;
    }

    const std::string& id() const { return id_; }
    std::string client_id() const {
        std::lock_guard lk(info_m_);
        return client_id_;
    }
    bool authed() const { return authed_; }
    TokenIdentity token() const {
        std::lock_guard lk(info_m_);
        return token_;
    }
    const std::shared_ptr<net::FrameConnection>& link() const { return conn_; }

    // Registers a session handle (any thread).
    void add_session(const std::shared_ptr<ServerSession>& s) {
        asio::post(conn_->strand(), [self = shared_from_this(), s] {
            self->handles_[s->handle()] = s;
            std::lock_guard lk(self->info_m_);
            self->session_count_ = self->handles_.size();
        });
    }
    void remove_session(std::uint32_t handle) {
        asio::post(conn_->strand(), [self = shared_from_this(), handle] {
            self->handles_.erase(handle);
            std::lock_guard lk(self->info_m_);
            self->session_count_ = self->handles_.size();
        });
    }

    void send_json(MsgType t, const json& j, std::uint32_t corr = 0) {
        conn_->send(encode_json(t, j, corr, corr != 0), true);
    }

private:
    void fail(ErrorCode code, const std::string& msg, std::uint32_t corr = 0) {
        conn_->send(encode_error(code, msg, corr), true);
        conn_->close_after_flush(Error{code, msg});
    }

    void on_frame(InFrame&& f) {
        const MsgType t = f.header.type;
        if (!authed_ && t != MsgType::Hello && t != MsgType::Auth && t != MsgType::Goodbye) {
            fail(ErrorCode::AuthRequired, "authenticate first", f.header.correlation);
            return;
        }
        switch (t) {
            case MsgType::Hello: return on_hello(f);
            case MsgType::Auth: return on_auth(f);
            case MsgType::Goodbye: conn_->close(Error{ErrorCode::Ok, "client goodbye"}); return;
            case MsgType::DeviceListRequest: return on_device_list(f);
            case MsgType::DeviceAttach: return on_attach(f);
            case MsgType::DeviceDetach: return on_detach(f);
            case MsgType::SessionResume: return on_resume(f);
            case MsgType::UsbSubmit: return on_submit(std::move(f));
            case MsgType::UsbCancel: {
                UsbCancelMsg m;
                if (decode_cancel(f.bytes(), m)) {
                    if (auto s = session(m.device)) s->cancel(m.seq);
                }
                return;
            }
            case MsgType::UsbReset: {
                UsbResetMsg m;
                if (!decode_reset(f.bytes(), m)) return fail(ErrorCode::MalformedPayload, "USB_RESET");
                if (auto s = session(m.device)) {
                    s->reset(m, [&eng = eng_](Status st) {
                        if (!st && st.error().code == ErrorCode::DeviceGone) eng.schedule_rescan(10);
                    });
                } else {
                    conn_->send(encode_op_result(UsbOpResultMsg{m.seq, m.device, UsbStatus::NoDevice}), true);
                }
                return;
            }
            case MsgType::UsbConfig: {
                UsbConfigMsg m;
                if (!decode_config(f.bytes(), m)) return fail(ErrorCode::MalformedPayload, "USB_CONFIG");
                if (auto s = session(m.device)) {
                    s->configure(m);
                } else {
                    conn_->send(encode_op_result(UsbOpResultMsg{m.seq, m.device, UsbStatus::NoDevice}), true);
                }
                return;
            }
            case MsgType::Error:
                log::warn("Network", "client " + client_name_ + " reported error: " +
                                         std::string(reinterpret_cast<const char*>(f.payload.data()), f.payload.size()));
                return;
            default:
                fail(ErrorCode::UnexpectedMessage, std::string("unexpected ") + std::string(to_string(t)),
                     f.header.correlation);
        }
    }

    std::shared_ptr<ServerSession> session(std::uint32_t handle) {
        auto it = handles_.find(handle);
        if (it == handles_.end()) return nullptr;
        if (it->second->state() == SessionState::Closed) {
            handles_.erase(it);
            return nullptr;
        }
        return it->second;
    }

    void on_submit(InFrame&& f) {
        UsbSubmitMsg m;
        if (auto st = decode_submit(f.bytes(), m); !st) {
            eng_.usb_totals_.errors.add();
            return fail(st.error().code, st.error().message);
        }
        auto s = session(m.device);
        if (!s) {
            UsbCompleteMsg c;
            c.seq = m.seq;
            c.device = m.device;
            c.status = UsbStatus::NoDevice;
            conn_->send(encode_complete(c));
            return;
        }
        s->submit(m, std::move(f));
    }

    void on_hello(const InFrame& f) {
        auto j = decode_json(f);
        if (!j) return fail(ErrorCode::MalformedPayload, j.error().message, f.header.correlation);
        bool v1 = false;
        if (auto it = j->find("protocol"); it != j->end() && it->is_array()) {
            for (const auto& v : *it) v1 |= v.is_number_integer() && v.get<int>() == kProtocolVersion;
        }
        if (!v1) return fail(ErrorCode::UnsupportedVersion, "no common protocol version", f.header.correlation);
        std::string cid = j->value("client_id", "");
        if (cid.empty() || cid.size() > 64) return fail(ErrorCode::InvalidArgument, "client_id", f.header.correlation);
        {
            std::lock_guard lk(info_m_);
            client_id_ = cid;
            client_name_ = j->value("client_name", "client");
            if (client_name_.size() > 64) client_name_.resize(64);
            app_version_ = j->value("app_version", "");
        }
        hello_ = true;
        const auto& c = eng_.cfg_;
        send_json(MsgType::HelloAck,
                  {{"protocol", kProtocolVersion},
                   {"server_id", eng_.server_id_},
                   {"server_name", c.server_name},
                   {"app_version", PF_VERSION_STRING},
                   {"auth_required", c.authentication},
                   {"capabilities", json::array({"iso", "resume", "reenumerate", "shared-readonly"})},
                   {"max_frame_size", c.network.max_frame_size},
                   {"keepalive_ms", c.keepalive.interval_ms},
                   {"reconnect_timeout_ms", c.reconnect_timeout_ms},
                   {"mobile_repair_mode", c.mobile_repair_mode}},
                  f.header.correlation);
        if (!c.authentication) {
            if (!conn_->is_secure()) log::warn("Security", "authentication disabled: client " + client_name_ + " accepted");
            complete_auth(TokenIdentity{"anonymous", "anonymous", false, {}}, 0);
        }
    }

    void on_auth(const InFrame& f) {
        if (!hello_) return fail(ErrorCode::ProtocolViolation, "HELLO required before AUTH", f.header.correlation);
        if (authed_) return send_json(MsgType::AuthResult, {{"ok", true}, {"client_id", client_id_}}, f.header.correlation);
        auto j = decode_json(f);
        if (!j) return fail(ErrorCode::MalformedPayload, j.error().message, f.header.correlation);
        auto r = eng_.tokens_.verify(j->value("token", ""));
        if (!r) {
            ++auth_failures_;
            LogRecord(LogLevel::Warning, "Security")
                .client(conn_->remote_address())
                .op("auth")
                .result("rejected")
                .error(static_cast<int>(ErrorCode::AuthFailed))
                .msg("authentication failed for " + client_name_);
            send_json(MsgType::AuthResult,
                      {{"ok", false}, {"code", static_cast<int>(ErrorCode::AuthFailed)}, {"message", "invalid token"}},
                      f.header.correlation);
            if (auth_failures_ >= kMaxAuthFailures) fail(ErrorCode::AuthFailed, "too many failed attempts");
            return;
        }
        complete_auth(*r, f.header.correlation);
    }

    void complete_auth(const TokenIdentity& tok, std::uint32_t corr) {
        // client limits (config + license), counted per distinct client id
        const std::size_t clients = eng_.count_clients_except(id_, client_id_);
        if (clients + 1 > eng_.cfg_.max_clients) return fail(ErrorCode::TooManyClients, "server client limit", corr);
        if (!eng_.license_.allows_clients(static_cast<std::uint32_t>(clients + 1)))
            return fail(ErrorCode::LicenseLimit, "license client limit reached", corr);
        {
            std::lock_guard lk(info_m_);
            token_ = tok;
        }
        authed_ = true;
        auth_timer_.cancel();
        if (corr || eng_.cfg_.authentication)
            send_json(MsgType::AuthResult, {{"ok", true}, {"client_id", client_id_}, {"admin", tok.admin}}, corr);
        LogRecord(LogLevel::Info, "Network")
            .client(client_name_)
            .op("connect")
            .result("ok")
            .msg("client " + client_name_ + " (" + conn_->remote_address() + ") authenticated" +
                 (conn_->is_secure() ? " [TLS]" : " [PLAINTEXT]"));
        eng_.emit("client", json{{"event", "connected"}, {"name", client_name_}});
    }

    void on_device_list(const InFrame& f) {
        json arr = json::array();
        const auto tok = token();
        for (const auto& d : eng_.devices()) {
            if (eng_.client_may_see(tok, d)) arr.push_back(to_json(d));
        }
        send_json(MsgType::DeviceList, {{"devices", arr}}, f.header.correlation);
    }

    void on_attach(const InFrame& f) {
        auto j = decode_json(f);
        if (!j) return fail(ErrorCode::MalformedPayload, j.error().message, f.header.correlation);
        AccessMode access = AccessMode::Exclusive;
        SessionPriority prio = eng_.cfg_.mobile_repair_mode ? SessionPriority::High : SessionPriority::Normal;
        parse_access_mode(j->value("access", "exclusive"), access);
        parse_priority(j->value("priority", std::string(to_string(prio))), prio);
        eng_.attach(shared_from_this(), f.header.correlation, j->value("device_id", ""), access, prio,
                    j->value("mobile_repair", eng_.cfg_.mobile_repair_mode));
    }

    void on_detach(const InFrame& f) {
        auto j = decode_json(f);
        if (!j) return fail(ErrorCode::MalformedPayload, j.error().message, f.header.correlation);
        const std::uint32_t h = j->value("handle", 0u);
        auto s = session(h);
        if (!s) {
            send_json(MsgType::DeviceDetachResult,
                      {{"ok", false}, {"code", static_cast<int>(ErrorCode::NotFound)}, {"message", "unknown handle"}},
                      f.header.correlation);
            return;
        }
        handles_.erase(h);
        {
            std::lock_guard lk(info_m_);
            session_count_ = handles_.size();
        }
        s->state_machine().transition(SessionState::Detached, "detached by client");
        eng_.close_session(s, "detached by client");
        send_json(MsgType::DeviceDetachResult, {{"ok", true}}, f.header.correlation);
    }

    void on_resume(const InFrame& f) {
        auto j = decode_json(f);
        if (!j) return fail(ErrorCode::MalformedPayload, j.error().message, f.header.correlation);
        eng_.resume(shared_from_this(), f.header.correlation, j->value("session_id", ""), j->value("resume_token", ""));
    }

    void on_closed(const Error& e) {
        auth_timer_.cancel();
        LogRecord(e.code == ErrorCode::Ok ? LogLevel::Info : LogLevel::Warning, "Network")
            .client(client_name_)
            .op("disconnect")
            .result(e.describe())
            .msg("client " + (client_name_.empty() ? conn_->remote_address() : client_name_) + " disconnected");
        for (auto& [h, s] : handles_) {
            if (s->connection() == conn_ || !s->connection()) eng_.on_session_connection_lost(s);
        }
        handles_.clear();
        eng_.remove_connection(id_);
        eng_.emit("client", json{{"event", "disconnected"}, {"name", client_name_}});
    }

    ServerEngine& eng_;
    std::string id_;
    std::shared_ptr<net::FrameConnection> conn_;
    asio::steady_timer auth_timer_;
    std::int64_t connected_ms_;
    bool hello_ = false;
    std::atomic<bool> authed_{false};
    int auth_failures_ = 0;
    std::unordered_map<std::uint32_t, std::shared_ptr<ServerSession>> handles_;  // strand only

    mutable std::mutex info_m_;
    std::string client_id_, client_name_, app_version_;
    TokenIdentity token_;
    std::size_t session_count_ = 0;
};

// =====================================================================
// ServerEngine
// =====================================================================

ServerEngine::ServerEngine(ServerConfig cfg, std::unique_ptr<IUsbBackend> backend)
    : cfg_(std::move(cfg)), backend_(std::move(backend)) {
    IdentityMatchPolicy pol;
    pol.max_age_ms = cfg_.reconnect_timeout_ms;
    matcher_ = IdentityMatcher(pol);
}

ServerEngine::~ServerEngine() { stop(); }

void ServerEngine::set_event_handler(EventFn fn) {
    std::lock_guard lk(ev_m_);
    on_event_ = std::move(fn);
}

void ServerEngine::emit(const std::string& kind, const json& body) {
    EventFn fn;
    {
        std::lock_guard lk(ev_m_);
        fn = on_event_;
    }
    if (fn) fn(kind, body);
}

Status ServerEngine::start() {
    if (running_) return {};
    if (auto st = validate(cfg_); !st) return st;
    started_ms_ = unix_time_ms();
    server_id_ = crypto::random_hex(8);

    // security material
    tokens_.set_file(cfg_.tokens_file);
    if (auto st = tokens_.load(); !st) return st;
    if (cfg_.authentication && tokens_.empty())
        log::warn("Security", "no client tokens exist yet: create one (parsflash-server --create-token NAME)");
    if (cfg_.tls_enabled) {
        crypto::CertificateFiles files{cfg_.cert_file, cfg_.key_file};
        if (auto st = crypto::ensure_self_signed_certificate(files, "ParsFlash USB Server " + cfg_.server_name); !st)
            return st;
        auto fp = crypto::certificate_fingerprint_file(cfg_.cert_file);
        if (!fp) return fp.error();
        fingerprint_ = *fp;
        auto ctx = net::make_server_tls_context(cfg_.cert_file, cfg_.key_file);
        if (!ctx) return ctx.error();
        tls_ = *ctx;
    } else {
        log::warn("Security", "TLS DISABLED: USB traffic and tokens are sent in plaintext");
    }
    if (auto st = license_.load(cfg_.license_file); !st) log::warn("License", license_.status_message());

    // USB backend
    if (!backend_) {
        if (cfg_.usb.backend == "test" || cfg_.usb.test_devices > 0) {
            backend_ = make_loopback_backend(cfg_.usb.test_devices);
        } else {
#ifdef PF_HAVE_LIBUSB
            backend_ = make_libusb_backend(cfg_.usb.use_usbdk);
#else
            return {ErrorCode::NotSupported, "built without libusb: only the test backend is available"};
#endif
        }
    }
    if (auto st = backend_->start(); !st) return st;

    // runtime
    rt_ = std::make_unique<net::IoRuntime>(effective_worker_threads(cfg_.worker_threads), "pf-server-io");
    blocking_ = std::make_unique<asio::thread_pool>(4);
    registry_strand_ = std::make_unique<net::Strand>(rt_->make_strand());
    rescan_timer_ = std::make_unique<asio::steady_timer>(*registry_strand_);
    poll_timer_ = std::make_unique<asio::steady_timer>(*registry_strand_);
    metrics_timer_ = std::make_unique<asio::steady_timer>(rt_->context());

    net::ListenerOptions lo;
    lo.address = cfg_.listen_address;
    lo.port = cfg_.server_port;
    lo.tls = tls_;
    lo.tuning = cfg_.network;
    lo.keepalive = cfg_.keepalive;
    listener_ = std::make_shared<net::Listener>(*rt_, lo);
    if (auto st = listener_->start([this](net::TransportPtr t) { on_accept(std::move(t)); }); !st) {
        rt_->stop();
        backend_->stop();
        return st;
    }
    running_ = true;
    backend_->set_change_callback([this] { schedule_rescan(); });
    schedule_rescan(0);
    if (!backend_->has_change_notifications()) {
        // Poll for topology changes; Mobile Repair Mode polls faster so mode switches
        // are noticed quickly.
        const std::uint32_t interval =
            cfg_.mobile_repair_mode ? std::min<std::uint32_t>(cfg_.usb.poll_interval_ms, 500) : cfg_.usb.poll_interval_ms;
        auto tick = std::make_shared<std::function<void()>>();
        *tick = [this, interval, tick] {
            poll_timer_->expires_after(std::chrono::milliseconds(interval));
            poll_timer_->async_wait([this, tick](std::error_code ec) {
                if (ec || !running_) return;
                schedule_rescan(0);
                (*tick)();
            });
        };
        asio::post(*registry_strand_, [tick] { (*tick)(); });
    }
    sample_metrics();
    LogRecord(LogLevel::Info, "Server")
        .op("start")
        .result("ok")
        .msg("ParsFlash USB Server " + std::string(PF_VERSION_STRING) + " listening on " + cfg_.listen_address + ":" +
             std::to_string(listener_->port()) + (cfg_.tls_enabled ? " (TLS 1.3)" : " (PLAINTEXT)") + ", backend " +
             backend_->name() + ", " + license_.status_message());
    return {};
}

void ServerEngine::stop() {
    if (!running_.exchange(false)) return;
    if (listener_) listener_->stop();
    std::vector<std::shared_ptr<ClientConn>> conns;
    std::vector<std::shared_ptr<ServerSession>> sessions;
    {
        std::lock_guard lk(m_);
        for (auto& [id, c] : conns_) conns.push_back(c);
        for (auto& [id, s] : sessions_) sessions.push_back(s);
    }
    for (auto& c : conns) c->close("server shutting down");
    for (auto& s : sessions) {
        s->state_machine().transition(SessionState::Closed, "server shutdown");
        s->shutdown();
    }
    if (blocking_) blocking_->join();  // device closes complete before the backend stops
    if (rt_) rt_->stop();
    {
        std::lock_guard lk(m_);
        conns_.clear();
        sessions_.clear();
        devices_.clear();
    }
    if (backend_) backend_->stop();
    log::info("Server", "server stopped");
}

std::uint16_t ServerEngine::port() const noexcept { return listener_ ? listener_->port() : cfg_.server_port; }

// ------------------------------------------------------------ connections

void ServerEngine::on_accept(net::TransportPtr t) {
    if (!running_) return;
    net::ConnectionOptions opts;
    opts.tuning = cfg_.network;
    opts.keepalive = cfg_.keepalive;
    auto fc = net::FrameConnection::create(std::move(t), opts);
    auto id = "c" + std::to_string(next_conn_.fetch_add(1));
    auto cc = std::make_shared<ClientConn>(*this, id, fc);
    {
        std::lock_guard lk(m_);
        if (conns_.size() >= cfg_.max_clients * 2 + 4) {  // hard cap incl. unauthenticated
            fc->send(encode_error(ErrorCode::TooManyClients, "server busy"), true);
            fc->close_after_flush(Error{ErrorCode::TooManyClients, "server busy"});
            fc->start({});
            return;
        }
        conns_[id] = cc;
    }
    LogRecord(LogLevel::Debug, "Network").client(fc->remote_address()).op("accept").msg("incoming connection");
    cc->start();
}

void ServerEngine::remove_connection(const std::string& id) {
    std::lock_guard lk(m_);
    conns_.erase(id);
}

std::size_t ServerEngine::count_clients_except(const std::string& conn_id, const std::string& client_id) const {
    std::lock_guard lk(m_);
    std::vector<std::string> ids;
    for (const auto& [id, c] : conns_) {
        if (id == conn_id || !c->authed()) continue;
        auto cid = c->client_id();
        if (cid == client_id) continue;  // reconnect of the same client does not count twice
        if (std::find(ids.begin(), ids.end(), cid) == ids.end()) ids.push_back(cid);
    }
    return ids.size();
}

std::vector<ClientSnapshot> ServerEngine::clients() const {
    std::vector<ClientSnapshot> out;
    std::lock_guard lk(m_);
    for (const auto& [id, c] : conns_) out.push_back(c->snapshot());
    return out;
}

// ------------------------------------------------------------ registry

DeviceExecutor ServerEngine::device_executor(const std::string& port_path) {
    std::lock_guard lk(m_);
    auto it = dev_exec_.find(port_path);
    if (it == dev_exec_.end()) it = dev_exec_.emplace(port_path, asio::make_strand(blocking_->get_executor())).first;
    return it->second;
}

void ServerEngine::schedule_rescan(std::uint32_t delay_ms) {
    if (!running_ || !registry_strand_) return;
    if (rescan_pending_.exchange(true)) return;  // already scheduled (debounce)
    asio::post(*registry_strand_, [this, delay_ms] {
        rescan_timer_->expires_after(std::chrono::milliseconds(delay_ms));
        rescan_timer_->async_wait([this](std::error_code ec) {
            if (ec || !running_) {
                rescan_pending_ = false;
                return;
            }
            do_rescan();
        });
    });
}

void ServerEngine::rescan_now() { schedule_rescan(0); }

void ServerEngine::do_rescan() {
    if (rescan_running_.exchange(true)) {
        rescan_pending_ = false;
        schedule_rescan(50);
        return;
    }
    rescan_pending_ = false;
    // Enumeration may block (SetupAPI); run it off the I/O threads.
    asio::post(*blocking_, [this] {
        auto scan = backend_->enumerate();
        asio::post(*registry_strand_, [this, scan = std::move(scan)]() mutable {
            if (scan && running_) apply_scan(std::move(*scan));
            else if (!scan) log::warn("USB", "enumeration failed: " + scan.error().describe());
            rescan_running_ = false;
        });
    });
}

DeviceInfo ServerEngine::make_info(const EnumeratedDevice& e) const {
    DeviceInfo d;
    d.identity = e.identity;
    fill_from_descriptors(d, e.descriptors, detector_);
    if (!e.manufacturer.empty()) d.manufacturer = e.manufacturer;
    if (!e.product.empty()) d.product = e.product;
    if (d.identity.serial.empty()) d.identity.serial = e.identity.serial;
    d.driver = e.driver;
    d.access_error = e.access_error;
    d.id = make_device_id(d.identity);
    return d;
}

bool ServerEngine::is_initially_shared(const DeviceInfo& d) const {
    if (cfg_.usb.share_policy == "all") return true;
    for (const auto& s : cfg_.usb.shared) {
        if (s == d.id || s == d.identity.port_path || TokenStore::glob_match(s, format_vid_pid(d.identity.vid, d.identity.pid)))
            return true;
    }
    return false;
}

void ServerEngine::apply_scan(std::vector<EnumeratedDevice> scan) {
    const std::int64_t now = unix_time_ms();
    std::map<std::string, EnumeratedDevice> found;
    for (auto& e : scan) {
        DeviceInfo tmp;
        tmp.identity = e.identity;
        found.emplace(make_device_id(e.identity), std::move(e));
    }
    std::vector<DeviceInfo> removed, arrived;
    std::vector<std::shared_ptr<ServerSession>> lost;
    std::vector<std::string> arrived_ids;
    {
        std::lock_guard lk(m_);
        for (auto it = devices_.begin(); it != devices_.end();) {
            if (found.count(it->first)) {
                ++it;
                continue;
            }
            if (it->second.session && it->second.session->state() != SessionState::Closed)
                lost.push_back(it->second.session);
            removed.push_back(it->second.info);
            it = devices_.erase(it);
        }
        for (auto& [id, e] : found) {
            if (devices_.count(id)) continue;
            DeviceEntry entry;
            entry.info = make_info(e);
            entry.raw = std::move(e);
            entry.info.shared = is_initially_shared(entry.info);
            if (!entry.info.access_error.empty()) entry.info.state = DeviceShareState::Unavailable;
            else entry.info.state = entry.info.shared ? DeviceShareState::Shared : DeviceShareState::Available;
            arrived.push_back(entry.info);
            arrived_ids.push_back(id);
            devices_.emplace(id, std::move(entry));
        }
    }
    for (const auto& d : removed) {
        LogRecord(LogLevel::Info, "USB")
            .device(d.id)
            .op("removed")
            .msg(d.display_name() + " [" + format_vid_pid(d.identity.vid, d.identity.pid) + "] removed from " +
                 d.identity.port_path);
        broadcast_device_event("removed", d);
    }
    for (auto& s : lost) {
        const SessionState before = s->state();
        s->device_lost();
        s->state_machine().transition(SessionState::Reconnecting, "device removed (re-enumeration expected)");
        LogRecord(LogLevel::Info, "Sessions").session(s->id()).op("device-lost").msg(
            "device left; waiting up to " + std::to_string(cfg_.reconnect_timeout_ms) + " ms for it to return");
        if (auto conn = s->connection()) {
            conn->send(encode_json(MsgType::DeviceEvent,
                                   json{{"event", "removed"},
                                        {"session_id", s->id()},
                                        {"handle", s->handle()},
                                        {"reconnecting", true},
                                        {"device", to_json(s->device_info())}}),
                       true);
        }
        std::weak_ptr<ServerSession> w = s;
        s->arm_grace_timer(cfg_.reconnect_timeout_ms, [this, w] {
            if (auto ss = w.lock()) close_session(ss, "device did not return within reconnect timeout");
        });
        (void)before;
    }
    for (const auto& d : arrived) {
        LogRecord(LogLevel::Info, "USB")
            .device(d.id)
            .op("arrived")
            .msg(d.display_name() + " [" + format_vid_pid(d.identity.vid, d.identity.pid) + "] mode=" + d.mode.label +
                 " port=" + d.identity.port_path + " speed=" + std::string(to_string(d.identity.speed)));
        broadcast_device_event("arrived", d);
    }
    if (!arrived_ids.empty()) try_rebind_orphans(arrived_ids);
    (void)now;
}

void ServerEngine::try_rebind_orphans(const std::vector<std::string>& arrived_ids) {
    const std::int64_t now = unix_time_ms();
    for (const auto& id : arrived_ids) {
        std::shared_ptr<ServerSession> best;
        int best_score = -1000;
        DeviceInfo info;
        {
            std::lock_guard lk(m_);
            auto it = devices_.find(id);
            if (it == devices_.end() || it->second.session) continue;
            info = it->second.info;
            for (auto& [sid, s] : sessions_) {
                if (s->state() != SessionState::Reconnecting || s->has_device() || s->access() != AccessMode::Exclusive)
                    continue;
                bool reserved = false;  // already being re-bound to another arrival
                for (auto& [did, e] : devices_) reserved |= e.session == s;
                if (reserved) continue;
                auto m = matcher_.match(s->device_info().identity, s->device_lost_at_ms(), info.identity, now);
                if (m.matched && m.score > best_score) {
                    best = s;
                    best_score = m.score;
                }
            }
            if (!best) continue;
            if (!cfg_.usb.auto_capture_on_reenumeration) {
                log::info("Sessions", "device " + id + " matches session " + short_id(best->id()) +
                                          " but auto capture is disabled");
                continue;
            }
            it->second.session = best;  // reserve
            it->second.info.shared = true;
            it->second.info.state = DeviceShareState::InUse;
            it->second.info.owner = best->owner_client_id();
            it->second.info.owner_name = best->owner_name();
            it->second.info.session_id = best->id();
            info = it->second.info;
        }
        LogRecord(LogLevel::Info, "Sessions")
            .session(best->id())
            .device(id)
            .op("reenumerated")
            .msg("matched re-enumerated device (score " + std::to_string(best_score) + "), new mode " + info.mode.label);
        auto exec = device_executor(info.identity.port_path);
        DeviceIdentity ident;
        {
            std::lock_guard lk(m_);
            ident = devices_.count(id) ? devices_[id].raw.identity : info.identity;
        }
        asio::post(exec, [this, best, id, info, ident, exec] {
            auto dev = backend_->open(ident);
            asio::post(*registry_strand_, [this, best, id, info, exec, dev = std::move(dev)]() mutable {
                if (!dev) {
                    log::warn("Sessions", "re-capture of " + id + " failed: " + dev.error().describe());
                    std::lock_guard lk(m_);
                    if (auto it = devices_.find(id); it != devices_.end() && it->second.session == best) {
                        it->second.session = nullptr;
                        it->second.info.state = DeviceShareState::Shared;
                        it->second.info.access_error = dev.error().message;
                        it->second.info.owner.clear();
                        it->second.info.owner_name.clear();
                        it->second.info.session_id.clear();
                    }
                    return;
                }
                std::shared_ptr<IUsbDevice> d(std::move(dev.value()));
                if (best->state() != SessionState::Reconnecting) {  // closed meanwhile
                    asio::post(exec, [d] { d->close(); });
                    return;
                }
                best->bind_device(d, info, exec);
                {
                    std::lock_guard lk(m_);
                    ++total_reenumerations_;
                }
                auto conn = best->connection();
                if (conn) {
                    best->disarm_grace_timer();
                    best->state_machine().transition(SessionState::Active, "device re-enumerated: " + info.mode.label);
                    conn->send(encode_json(MsgType::DeviceReenumerated,
                                           json{{"session_id", best->id()},
                                                {"handle", best->handle()},
                                                {"speed", std::string(to_string(best->speed()))},
                                                {"descriptors", to_json(best->descriptors())},
                                                {"device", to_json(info)}}),
                               true);
                } else {
                    // Client is away; keep the grace timer running for its return.
                    best->state_machine().transition(SessionState::Suspended, "device back, client away");
                }
                broadcast_device_event("changed", info);
            });
        });
    }
}

std::vector<DeviceInfo> ServerEngine::devices() const {
    std::vector<DeviceInfo> out;
    std::lock_guard lk(m_);
    for (const auto& [id, e] : devices_) out.push_back(e.info);
    return out;
}

bool ServerEngine::client_may_see(const TokenIdentity& tok, const DeviceInfo& d) const {
    if (tok.admin || tok.allowed_devices.empty()) return true;
    const std::string vp = format_vid_pid(d.identity.vid, d.identity.pid);
    for (const auto& pat : tok.allowed_devices) {
        if (TokenStore::glob_match(pat, d.id) || TokenStore::glob_match(pat, vp) ||
            TokenStore::glob_match(pat, d.identity.port_path))
            return true;
    }
    return false;
}

void ServerEngine::broadcast_device_event(const std::string& event, const DeviceInfo& d) {
    std::vector<std::shared_ptr<ClientConn>> conns;
    {
        std::lock_guard lk(m_);
        for (auto& [id, c] : conns_) conns.push_back(c);
    }
    const json body{{"event", event}, {"device", to_json(d)}};
    for (auto& c : conns) {
        if (c->authed() && client_may_see(c->token(), d)) c->send_json(MsgType::DeviceEvent, body);
    }
    emit("device", body);
}

// ------------------------------------------------------------ sessions

void ServerEngine::attach(const std::shared_ptr<ClientConn>& cc, std::uint32_t corr, const std::string& device_id,
                          AccessMode access, SessionPriority prio, bool mobile_repair) {
    auto reply_error = [&](ErrorCode code, const std::string& msg) {
        cc->send_json(MsgType::DeviceAttachResult, {{"ok", false}, {"code", static_cast<int>(code)}, {"message", msg}},
                      corr);
        LogRecord(LogLevel::Warning, "Sessions").client(cc->snapshot().name).device(device_id).op("attach").result(
            std::string(to_string(code))).msg(msg);
    };
    const auto snap = cc->snapshot();
    const auto tok = cc->token();
    std::shared_ptr<ServerSession> session, takeover;
    DeviceInfo info;
    DeviceIdentity ident;
    {
        std::lock_guard lk(m_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return reply_error(ErrorCode::DeviceNotFound, "no such device");
        DeviceEntry& e = it->second;
        if (!client_may_see(tok, e.info)) return reply_error(ErrorCode::AccessDenied, "device not permitted for this token");
        if (!e.info.shared) return reply_error(ErrorCode::AccessDenied, "device is not shared");
        if (e.info.state == DeviceShareState::Unavailable)
            return reply_error(ErrorCode::DeviceAccess, e.info.access_error);
        if (access == AccessMode::Exclusive) {
            if (e.session && e.session->state() != SessionState::Closed) {
                const auto st = e.session->state();
                if (e.session->owner_client_id() == snap.client_id &&
                    (st == SessionState::Suspended || st == SessionState::Reconnecting || st == SessionState::Active)) {
                    takeover = e.session;  // same client lost its session state (e.g. client restart)
                } else {
                    return reply_error(ErrorCode::DeviceBusy, "device in use by " + e.session->owner_name());
                }
            }
            std::uint32_t attached = 0;
            for (auto& [sid, s] : sessions_)
                attached += s->access() == AccessMode::Exclusive && s->state() != SessionState::Closed;
            if (!takeover && attached + 1 > cfg_.max_devices)
                return reply_error(ErrorCode::TooManyDevices, "server device limit reached");
            if (!takeover && !license_.allows_devices(attached + 1))
                return reply_error(ErrorCode::LicenseLimit, "license device limit reached");
        }
        ServerSession::Params p;
        p.id = crypto::random_hex(16);
        p.resume_token = crypto::random_hex(32);
        p.owner_client_id = snap.client_id;
        p.owner_name = snap.name;
        p.handle = next_handle_.fetch_add(1);
        p.access = access;
        p.priority = prio;
        p.mobile_repair = mobile_repair;
        session = std::make_shared<ServerSession>(p, rt_->context());
        session->state_machine().transition(SessionState::Attaching, "attach requested");
        sessions_[p.id] = session;
        if (access == AccessMode::Exclusive) {
            e.session = session;
            e.info.state = DeviceShareState::InUse;
            e.info.owner = snap.client_id;
            e.info.owner_name = snap.name;
            e.info.session_id = p.id;
        } else {
            e.observers.push_back(session);
        }
        info = e.info;
        ident = e.raw.identity;
    }
    std::shared_ptr<IUsbDevice> old_dev;
    if (takeover) {
        old_dev = takeover->release_device();
        {
            std::lock_guard lk(m_);
            sessions_.erase(takeover->id());
        }
        takeover->state_machine().transition(SessionState::Closed, "taken over by a new attach of the same client");
        takeover->shutdown();
        log::info("Sessions", "client " + snap.name + " re-attached; previous session " + short_id(takeover->id()) +
                                  " replaced");
    }

    auto finish_ok = [this, cc, corr, session, info](std::shared_ptr<IUsbDevice> dev, std::optional<DeviceExecutor> ex) {
        DeviceInfo i = info;
        session->bind_device(dev, i, std::move(ex));
        session->set_connection(cc->link());
        cc->add_session(session);
        session->state_machine().transition(SessionState::Active, "attached");
        DescriptorSet desc = dev ? dev->descriptors() : DescriptorSet{};
        if (!dev) {
            std::lock_guard lk(m_);
            if (auto it = devices_.find(i.id); it != devices_.end()) desc = it->second.raw.descriptors;
        }
        cc->send_json(MsgType::DeviceAttachResult,
                      {{"ok", true},
                       {"handle", session->handle()},
                       {"session_id", session->id()},
                       {"resume_token", session->resume_token()},
                       {"speed", std::string(to_string(dev ? dev->speed() : i.identity.speed))},
                       {"access", std::string(to_string(session->access()))},
                       {"priority", std::string(to_string(session->priority()))},
                       {"descriptors", to_json(desc)},
                       {"device", to_json(i)}},
                      corr);
        LogRecord(LogLevel::Info, "Sessions")
            .session(session->id())
            .client(session->owner_name())
            .device(i.id)
            .op("attach")
            .result("ok")
            .msg(i.display_name() + " attached by " + session->owner_name() + " (" +
                 std::string(to_string(session->access())) + ", priority " +
                 std::string(to_string(session->priority())) + ")");
        broadcast_device_event("changed", i);
        emit("session", session->to_json());
    };

    if (access == AccessMode::SharedReadOnly) {
        finish_ok(nullptr, std::nullopt);
        return;
    }
    auto exec = device_executor(ident.port_path);
    asio::post(exec, [this, cc, corr, session, ident, info, old_dev, exec, finish_ok]() mutable {
        if (old_dev) old_dev->close();
        const std::uint64_t t0 = monotonic_ns();
        auto dev = backend_->open(ident);
        const std::int64_t us = static_cast<std::int64_t>((monotonic_ns() - t0) / 1000);
        asio::post(cc->link()->strand(), [this, cc, corr, session, info, exec, dev = std::move(dev), us,
                                          finish_ok]() mutable {
            if (!dev) {
                session->state_machine().transition(SessionState::Failed, dev.error().describe());
                {
                    std::lock_guard lk(m_);
                    if (auto it = devices_.find(info.id); it != devices_.end()) it->second.info.access_error = dev.error().message;
                }
                close_session(session, "capture failed");
                cc->send_json(MsgType::DeviceAttachResult,
                              {{"ok", false},
                               {"code", static_cast<int>(dev.error().code)},
                               {"message", dev.error().message}},
                              corr);
                LogRecord(LogLevel::Error, "USB").device(info.id).op("capture").duration_us(us).result("failed").error(
                    static_cast<int>(dev.error().code)).msg(dev.error().describe());
                return;
            }
            if (session->state() != SessionState::Attaching) {  // client vanished meanwhile
                std::shared_ptr<IUsbDevice> d(std::move(dev.value()));
                asio::post(exec, [d] { d->close(); });
                return;
            }
            LogRecord(LogLevel::Debug, "USB").device(info.id).op("capture").duration_us(us).result("ok");
            finish_ok(std::shared_ptr<IUsbDevice>(std::move(dev.value())), exec);
        });
    });
}

void ServerEngine::resume(const std::shared_ptr<ClientConn>& cc, std::uint32_t corr, const std::string& session_id,
                          const std::string& token) {
    auto reply_error = [&](ErrorCode code, const std::string& msg) {
        cc->send_json(MsgType::SessionResumeResult,
                      {{"ok", false}, {"code", static_cast<int>(code)}, {"message", msg}, {"session_id", session_id}},
                      corr);
    };
    std::shared_ptr<ServerSession> s;
    {
        std::lock_guard lk(m_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return reply_error(ErrorCode::SessionExpired, "session expired or unknown");
        s = it->second;
    }
    if (!crypto::constant_time_equal(token, s->resume_token()) || s->owner_client_id() != cc->client_id())
        return reply_error(ErrorCode::AccessDenied, "resume token rejected");
    const SessionState st = s->state();
    if (st == SessionState::Closed || st == SessionState::Failed || st == SessionState::Attaching)
        return reply_error(ErrorCode::SessionExpired, "session not resumable");
    if (auto old = s->connection(); old && old != cc->link()) old->close(Error{ErrorCode::Ok, "superseded by resume"});
    s->connection_lost();  // drop any stale in-flight state
    s->set_connection(cc->link());
    cc->add_session(s);
    {
        std::lock_guard lk(m_);
        ++total_reconnects_;
    }
    if (s->has_device() || s->access() == AccessMode::SharedReadOnly) {
        s->disarm_grace_timer();
        s->state_machine().transition(SessionState::Active, "session resumed");
    }
    const DeviceInfo info = s->device_info();
    cc->send_json(MsgType::SessionResumeResult,
                  {{"ok", true},
                   {"session_id", s->id()},
                   {"handle", s->handle()},
                   {"state", std::string(to_string(s->state()))},
                   {"speed", std::string(to_string(s->speed()))},
                   {"descriptors", to_json(s->descriptors())},
                   {"device", to_json(info)}},
                  corr);
    LogRecord(LogLevel::Info, "Sessions").session(s->id()).client(s->owner_name()).op("resume").result("ok").msg(
        "session resumed by " + s->owner_name());
    emit("session", s->to_json());
}

void ServerEngine::on_session_connection_lost(const std::shared_ptr<ServerSession>& s) {
    s->connection_lost();
    if (s->state() == SessionState::Active || s->state() == SessionState::Attaching) {
        if (!s->state_machine().transition(SessionState::Suspended, "network connection lost")) {
            close_session(s, "connection lost while attaching");
            return;
        }
    }
    LogRecord(LogLevel::Warning, "Sessions").session(s->id()).client(s->owner_name()).op("suspend").msg(
        "connection lost; session kept for " + std::to_string(cfg_.reconnect_timeout_ms) + " ms");
    std::weak_ptr<ServerSession> w = s;
    s->arm_grace_timer(cfg_.reconnect_timeout_ms, [this, w] {
        if (auto ss = w.lock()) close_session(ss, "client did not reconnect within reconnect timeout");
    });
    emit("session", s->to_json());
}

void ServerEngine::close_session(const std::shared_ptr<ServerSession>& s, const std::string& reason) {
    DeviceInfo changed;
    bool device_changed = false;
    {
        std::lock_guard lk(m_);
        sessions_.erase(s->id());
        for (auto& [id, e] : devices_) {
            if (e.session == s) {
                e.session = nullptr;
                e.info.state = e.info.shared ? DeviceShareState::Shared : DeviceShareState::Available;
                e.info.owner.clear();
                e.info.owner_name.clear();
                e.info.session_id.clear();
                changed = e.info;
                device_changed = true;
            }
            e.observers.erase(std::remove_if(e.observers.begin(), e.observers.end(),
                                             [&](const std::weak_ptr<ServerSession>& w) {
                                                 auto o = w.lock();
                                                 return !o || o == s;
                                             }),
                              e.observers.end());
        }
    }
    if (auto conn = s->connection()) {
        conn->send(encode_json(MsgType::DeviceEvent,
                               json{{"event", "session-closed"},
                                    {"session_id", s->id()},
                                    {"handle", s->handle()},
                                    {"reason", reason}}),
                   true);
    }
    s->state_machine().transition(SessionState::Closed, reason);
    s->shutdown();
    LogRecord(LogLevel::Info, "Sessions").session(s->id()).client(s->owner_name()).op("close").msg(reason);
    if (device_changed) broadcast_device_event("changed", changed);
    emit("session", s->to_json());
}

Result<std::shared_ptr<ServerSession>> ServerEngine::find_session(const std::string& id) {
    std::lock_guard lk(m_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return Error{ErrorCode::NotFound, "session"};
    return it->second;
}

// ------------------------------------------------------------ management

Status ServerEngine::share(const std::string& device_id, bool shared) {
    std::shared_ptr<ServerSession> victim;
    DeviceInfo info;
    {
        std::lock_guard lk(m_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return {ErrorCode::DeviceNotFound, device_id};
        auto& e = it->second;
        e.info.shared = shared;
        if (!shared && e.session) victim = e.session;
        if (!e.session) e.info.state = shared ? DeviceShareState::Shared : DeviceShareState::Available;
        if (shared) e.info.access_error.clear();
        info = e.info;
    }
    if (victim) close_session(victim, "sharing stopped by server");
    LogRecord(LogLevel::Info, "USB").device(device_id).op(shared ? "share" : "unshare").msg(info.display_name());
    broadcast_device_event("changed", info);
    return {};
}

Status ServerEngine::reset_device(const std::string& device_id) {
    std::shared_ptr<ServerSession> s;
    DeviceIdentity ident;
    {
        std::lock_guard lk(m_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return {ErrorCode::DeviceNotFound, device_id};
        s = it->second.session;
        ident = it->second.raw.identity;
    }
    if (s) {
        s->reset(UsbResetMsg{0, s->handle(), ResetKind::PortReset}, [this](Status st) {
            if (!st && st.error().code == ErrorCode::DeviceGone) schedule_rescan(10);
        });
        return {};
    }
    // Not attached: capture briefly, reset, release.
    auto exec = device_executor(ident.port_path);
    asio::post(exec, [this, ident, device_id] {
        auto dev = backend_->open(ident);
        if (!dev) {
            log::warn("USB", "reset " + device_id + ": " + dev.error().describe());
            return;
        }
        auto st = (*dev)->reset_port();
        (*dev)->close();
        LogRecord(st.ok() ? LogLevel::Info : LogLevel::Warning, "USB").device(device_id).op("reset").result(
            st.ok() ? "ok" : st.error().describe());
        schedule_rescan(100);
    });
    return {};
}

Status ServerEngine::detach_device(const std::string& device_id, const std::string& reason) {
    std::shared_ptr<ServerSession> s;
    {
        std::lock_guard lk(m_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return {ErrorCode::DeviceNotFound, device_id};
        s = it->second.session;
    }
    if (!s) return {ErrorCode::InvalidState, "device is not attached"};
    s->state_machine().transition(SessionState::Detached, reason);
    close_session(s, reason);
    return {};
}

json ServerEngine::sessions_json() const {
    json arr = json::array();
    std::lock_guard lk(m_);
    for (const auto& [id, s] : sessions_) arr.push_back(s->to_json());
    return arr;
}

void ServerEngine::sample_metrics() {
    if (!running_ || !metrics_timer_) return;
    const std::uint64_t now = monotonic_ns();
    std::vector<std::shared_ptr<ServerSession>> ss;
    std::vector<std::shared_ptr<ClientConn>> cs;
    {
        std::lock_guard lk(m_);
        for (auto& [id, s] : sessions_) ss.push_back(s);
        for (auto& [id, c] : conns_) cs.push_back(c);
    }
    for (auto& s : ss) s->metrics().sample(now);
    for (auto& c : cs) c->link()->metrics()->sample(now);
    {
        std::lock_guard lk(m_);
        cpu_percent_ = cpu_.sample();
    }
    metrics_timer_->expires_after(std::chrono::seconds(1));
    metrics_timer_->async_wait([this](std::error_code ec) {
        if (!ec) sample_metrics();
    });
}

json ServerEngine::status_json() const {
    const auto usage = sample_process_usage();
    std::lock_guard lk(m_);
    std::size_t shared = 0, in_use = 0, authed = 0;
    for (const auto& [id, e] : devices_) {
        shared += e.info.shared;
        in_use += e.info.state == DeviceShareState::InUse;
    }
    double tx = 0, rx = 0;
    for (const auto& [id, c] : conns_) {
        authed += c->authed();
        tx += c->link()->metrics()->tx.rate() * 8;
        rx += c->link()->metrics()->rx.rate() * 8;
    }
    return {{"product", "ParsFlash USB Redirector"},
            {"component", "server"},
            {"version", PF_VERSION_STRING},
            {"website", "https://parsflash.com"},
            {"server_name", cfg_.server_name},
            {"server_id", server_id_},
            {"online", running_.load()},
            {"listen_address", cfg_.listen_address},
            {"port", listener_ ? listener_->port() : cfg_.server_port},
            {"tls", cfg_.tls_enabled},
            {"fingerprint", fingerprint_},
            {"authentication", cfg_.authentication},
            {"uptime_s", (unix_time_ms() - started_ms_) / 1000},
            {"backend", backend_ ? backend_->name() : ""},
            {"devices", devices_.size()},
            {"shared_devices", shared},
            {"devices_in_use", in_use},
            {"clients", authed},
            {"connections", conns_.size()},
            {"sessions", sessions_.size()},
            {"upload_bps", tx},
            {"download_bps", rx},
            {"reconnects", total_reconnects_},
            {"reenumerations", total_reenumerations_},
            {"mobile_repair_mode", cfg_.mobile_repair_mode},
            {"performance_mode", std::string(to_string(cfg_.performance_mode))},
            {"cpu_percent", cpu_percent_},
            {"ram_bytes", usage.rss_bytes},
            {"license", license_.to_json()}};
}

json ServerEngine::metrics_json() const {
    json sessions = json::array(), links = json::array();
    std::vector<std::shared_ptr<ServerSession>> ss;
    std::vector<std::shared_ptr<ClientConn>> cs;
    {
        std::lock_guard lk(m_);
        for (auto& [id, s] : sessions_) ss.push_back(s);
        for (auto& [id, c] : conns_) cs.push_back(c);
    }
    std::uint64_t timeouts = 0, errors = 0, resets = 0, transfers = 0;
    for (auto& s : ss) {
        sessions.push_back(s->to_json());
        timeouts += s->metrics().timeouts.get();
        errors += s->metrics().errors.get();
        resets += s->metrics().resets.get();
        transfers += s->metrics().transfers.get();
    }
    for (auto& c : cs) {
        auto j = to_json(*c->link()->metrics());
        j["client"] = c->snapshot().name;
        j["address"] = c->link()->remote_address();
        links.push_back(j);
    }
    const auto bp = BufferPool::global().stats();
    const auto lg = Logger::instance().stats();
    return {{"status", status_json()},
            {"totals", {{"transfers", transfers}, {"timeouts", timeouts}, {"errors", errors}, {"resets", resets}}},
            {"sessions", sessions},
            {"links", links},
            {"buffer_pool",
             {{"acquisitions", bp.acquisitions}, {"hits", bp.hits}, {"cached_bytes", bp.cached_bytes},
              {"outstanding", bp.outstanding}}},
            {"logger", {{"written", lg.written}, {"dropped", lg.dropped}}}};
}

}  // namespace pf::server
