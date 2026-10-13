#include "Core/ClientEngine.hpp"

#include <algorithm>
#include <future>
#include <set>

#include "Common/Platform.hpp"
#include "Crypto/Crypto.hpp"
#include "Crypto/TokenStore.hpp"
#include "Logging/Logger.hpp"

namespace pf::client {

using namespace proto;
using nlohmann::json;

std::string_view to_string(LinkState s) noexcept {
    switch (s) {
        case LinkState::Disconnected: return "disconnected";
        case LinkState::Connecting: return "connecting";
        case LinkState::Authenticating: return "authenticating";
        case LinkState::Connected: return "connected";
        case LinkState::Reconnecting: return "reconnecting";
        case LinkState::Failed: return "failed";
    }
    return "disconnected";
}

// =====================================================================
// ServerLink: connection to one ParsFlash USB Server. Owns reconnects.
// All mutable state is touched on the link strand.
// =====================================================================
class ServerLink : public std::enable_shared_from_this<ServerLink> {
public:
    using Reply = std::function<void(Result<json>)>;

    ServerLink(ClientEngine& eng, ServerEntry entry)
        : eng_(eng), entry_(std::move(entry)), strand_(eng.rt_->make_strand()), timer_(strand_),
          metrics_(std::make_shared<LinkMetrics>()) {}

    // ---------------------------------------------------------------- control
    void connect() {
        asio::post(strand_, [self = shared_from_this()] {
            self->user_disconnect_ = false;
            if (self->state_ == LinkState::Connected || self->state_ == LinkState::Connecting ||
                self->state_ == LinkState::Authenticating)
                return;
            self->attempt_ = 0;
            self->do_connect();
        });
    }

    void disconnect() {
        auto self = shared_from_this();
        std::promise<void> done;
        auto fut = done.get_future();
        asio::post(strand_, [self, &done] {
            self->user_disconnect_ = true;
            self->timer_.cancel();
            if (self->conn_) {
                self->conn_->send(encode_json(MsgType::Goodbye, json{{"reason", "client disconnect"}}), true);
                self->conn_->close_after_flush(Error{ErrorCode::Ok, "disconnect"});
            }
            self->set_state(LinkState::Disconnected, "");
            done.set_value();
        });
        fut.wait_for(std::chrono::seconds(5));
    }

    void drop_for_test() {
        asio::post(strand_, [self = shared_from_this()] {
            if (self->conn_) self->conn_->close(Error{ErrorCode::ConnectionLost, "simulated network loss"});
        });
    }

    // ---------------------------------------------------------------- requests
    void request(MsgType type, json body, std::chrono::milliseconds timeout, Reply cb) {
        asio::post(strand_, [self = shared_from_this(), type, body = std::move(body), timeout, cb = std::move(cb)]() mutable {
            if (!self->conn_ || self->state_ != LinkState::Connected) {
                cb(Error{ErrorCode::ConnectionLost, "not connected to " + self->entry_.name});
                return;
            }
            self->send_request(type, std::move(body), timeout, std::move(cb));
        });
    }

    void attach_session(const std::shared_ptr<ClientSession>& s) {
        asio::post(strand_, [self = shared_from_this(), s] { self->sessions_[s->handle()] = s; });
    }
    void forget_session(std::uint32_t handle) {
        asio::post(strand_, [self = shared_from_this(), handle] { self->sessions_.erase(handle); });
    }

    // ---------------------------------------------------------------- views
    ServerSnapshot snapshot() const {
        std::lock_guard lk(snap_m_);
        ServerSnapshot s;
        s.name = entry_.name;
        s.host = entry_.host;
        s.port = entry_.port;
        s.tls = entry_.tls;
        s.state = pub_state_;
        s.last_error = last_error_;
        s.fingerprint = entry_.fingerprint;
        s.server_name = server_name_;
        s.server_version = server_version_;
        s.rtt_us = metrics_->last_rtt_us.load();
        s.tx_bps = metrics_->tx.rate() * 8;
        s.rx_bps = metrics_->rx.rate() * 8;
        s.reconnects = metrics_->reconnects.get();
        s.packet_loss = metrics_->packet_loss();
        s.devices = devices_.size();
        s.sessions = session_count_;
        return s;
    }
    std::vector<DeviceInfo> devices() const {
        std::lock_guard lk(snap_m_);
        return devices_;
    }
    ServerEntry entry() const {
        std::lock_guard lk(snap_m_);
        return entry_;
    }
    LinkMetrics& metrics() { return *metrics_; }
    net::Strand& strand() { return strand_; }
    std::shared_ptr<net::FrameConnection> connection() const { return conn_; }  // strand only

private:
    // ---------------------------------------------------------------- state
    void set_state(LinkState s, const std::string& err) {
        state_ = s;
        {
            std::lock_guard lk(snap_m_);
            pub_state_ = s;
            if (!err.empty() || s == LinkState::Connected) last_error_ = err;
        }
        eng_.publish_event("server", json{{"name", entry_.name}, {"state", std::string(to_string(s))}, {"error", err}});
    }

    std::chrono::milliseconds backoff() {
        const auto& r = eng_.cfg_.reconnect;
        double base = r.initial_delay_ms;
        double cap = r.max_delay_ms;
        if (eng_.cfg_.mobile_repair_mode) {  // aggressive reconnect
            base = std::min(base, 100.0);
            cap = std::min(cap, 2000.0);
        }
        double d = base;
        for (std::uint32_t i = 0; i < attempt_ && d < cap; ++i) d *= r.multiplier;
        if (d > cap) d = cap;
        // +-20% jitter so many clients do not reconnect in lockstep
        std::uint8_t j;
        crypto::random_bytes(MutableByteSpan(&j, 1));
        d *= 0.8 + 0.4 * (j / 255.0);
        return std::chrono::milliseconds(static_cast<long>(d));
    }

    void schedule_reconnect(const std::string& why) {
        if (user_disconnect_ || !eng_.running_) {
            set_state(LinkState::Disconnected, why);
            return;
        }
        if (!eng_.cfg_.auto_reconnect) {
            set_state(LinkState::Failed, why);
            return;
        }
        set_state(LinkState::Reconnecting, why);
        const auto d = backoff();
        ++attempt_;
        timer_.expires_after(d);
        timer_.async_wait([self = shared_from_this()](std::error_code ec) {
            if (!ec && !self->user_disconnect_) self->do_connect();
        });
    }

    void do_connect() {
        set_state(LinkState::Connecting, "");
        net::ConnectOptions co;
        co.host = entry_.host;
        co.port = entry_.port;
        co.tls = entry_.tls;
        co.expected_fingerprint = entry_.fingerprint;
        co.tuning = eng_.cfg_.network;
        co.keepalive = eng_.cfg_.keepalive;
        co.strand = strand_;  // connection callbacks run on the link strand
        net::async_connect(*eng_.rt_, co, [self = shared_from_this()](Result<net::TransportPtr> r) {
            asio::post(self->strand_, [self, r = std::move(r)]() mutable { self->on_connected(std::move(r)); });
        });
    }

    void on_connected(Result<net::TransportPtr> r) {
        if (user_disconnect_) {
            if (r) (*r)->close();
            return;
        }
        if (!r) {
            LogRecord(LogLevel::Warning, "Network").client(entry_.name).op("connect").result(r.error().describe());
            if (r.error().code == ErrorCode::TlsFailure && r.error().message.find("fingerprint") != std::string::npos) {
                set_state(LinkState::Failed, r.error().message);  // never auto-retry a possible MITM
                return;
            }
            schedule_reconnect(r.error().describe());
            return;
        }
        net::ConnectionOptions opts;
        opts.tuning = eng_.cfg_.network;
        opts.keepalive = eng_.cfg_.keepalive;
        conn_ = net::FrameConnection::create(std::move(r.value()), opts, metrics_);
        if (entry_.tls && entry_.fingerprint.empty()) {
            // Trust on first use: pin the fingerprint we saw; later mismatches fail.
            {
                std::lock_guard lk(snap_m_);
                entry_.fingerprint = conn_->peer_fingerprint();
            }
            LogRecord(LogLevel::Warning, "Security")
                .client(entry_.name)
                .op("trust")
                .msg("pinned server certificate " + entry_.fingerprint + " (trust on first use)");
            eng_.publish_event("trust", json{{"name", entry_.name}, {"fingerprint", entry_.fingerprint}});
        }
        if (!entry_.tls) log::warn("Security", "connection to " + entry_.name + " is NOT encrypted");
        auto conn = conn_;
        std::weak_ptr<ServerLink> w = shared_from_this();
        conn->start({[w, conn](InFrame&& f) {
                         if (auto self = w.lock(); self && self->conn_ == conn) self->on_frame(std::move(f));
                     },
                     [w, conn](const Error& e) {
                         if (auto self = w.lock()) {
                             asio::post(self->strand_, [self, conn, e] {
                                 if (self->conn_ == conn) self->on_closed(e);
                             });
                         }
                     },
                     nullptr});
        set_state(LinkState::Authenticating, "");
        send_request(MsgType::Hello,
                     json{{"protocol", json::array({kProtocolVersion})},
                          {"client_id", eng_.cfg_.client_id},
                          {"client_name", eng_.cfg_.client_name},
                          {"app_version", PF_VERSION_STRING},
                          {"capabilities", json::array({"iso", "resume", "reenumerate"})}},
                     std::chrono::seconds(10), [self = shared_from_this()](Result<json> j) { self->on_hello(std::move(j)); });
    }

    void on_hello(Result<json> j) {
        if (!j) return fail_connection(j.error());
        {
            std::lock_guard lk(snap_m_);
            server_name_ = j->value("server_name", "");
            server_version_ = j->value("app_version", "");
        }
        if (!j->value("auth_required", true)) return on_authenticated();
        const std::string token = eng_.tokens_ ? eng_.tokens_(entry_) : std::string();
        send_request(MsgType::Auth, json{{"method", "token"}, {"token", token}}, std::chrono::seconds(10),
                     [self = shared_from_this()](Result<json> r) {
                         if (!r) return self->fail_connection(r.error());
                         if (!r->value("ok", false)) {
                             // Wrong token: do not hammer the server with retries.
                             self->user_disconnect_ = true;
                             if (self->conn_) self->conn_->close(Error{ErrorCode::AuthFailed, "authentication rejected"});
                             self->set_state(LinkState::Failed, "authentication rejected: " + r->value("message", ""));
                             return;
                         }
                         self->on_authenticated();
                     });
    }

    void on_authenticated() {
        if (attempt_ > 0 || had_connection_) metrics_->reconnects.add();
        attempt_ = 0;
        had_connection_ = true;
        set_state(LinkState::Connected, "");
        LogRecord(LogLevel::Info, "Network")
            .client(entry_.name)
            .op("connect")
            .result("ok")
            .msg("connected to " + entry_.name + " (" + conn_->remote_address() + ")" +
                 (conn_->is_secure() ? " [TLS 1.3]" : " [PLAINTEXT]"));
        refresh_devices();
        resume_sessions();
    }

    void fail_connection(const Error& e) {
        if (conn_) conn_->close(e);
    }

    void on_closed(const Error& e) {
        // fail outstanding requests
        auto pend = std::move(pending_);
        pending_.clear();
        for (auto& [corr, p] : pend) {
            p.timer->cancel();
            p.cb(Error{ErrorCode::ConnectionLost, "connection lost"});
        }
        conn_.reset();
        for (auto& [h, s] : sessions_) {
            s->link_down();
            arm_session_expiry(s);
        }
        LogRecord(e.code == ErrorCode::Ok ? LogLevel::Info : LogLevel::Warning, "Network")
            .client(entry_.name)
            .op("disconnect")
            .result(e.describe());
        if (state_ == LinkState::Failed) return;
        schedule_reconnect(e.describe());
    }

    // Local counterpart of the server grace period: unplug if not resumed in time.
    void arm_session_expiry(const std::shared_ptr<ClientSession>& s) {
        auto t = std::make_shared<asio::steady_timer>(strand_, std::chrono::milliseconds(eng_.cfg_.reconnect_timeout_ms));
        std::weak_ptr<ClientSession> ws = s;
        t->async_wait([self = shared_from_this(), ws, t](std::error_code ec) {
            if (ec) return;
            auto ss = ws.lock();
            if (!ss || ss->state() != SessionState::Suspended) return;
            log::warn("Sessions", "session " + ss->id().substr(0, 8) + " not resumed within reconnect timeout");
            self->sessions_.erase(ss->handle());
            ss->close("reconnect timeout");
            self->eng_.unregister_session(ss);
        });
    }

    void resume_sessions() {
        for (auto& [h, s] : sessions_) {
            if (s->state() != SessionState::Suspended && s->state() != SessionState::Reconnecting) continue;
            std::weak_ptr<ClientSession> ws = s;
            send_request(MsgType::SessionResume, json{{"session_id", s->id()}, {"resume_token", s->resume_token()}},
                         std::chrono::seconds(15), [self = shared_from_this(), ws](Result<json> r) {
                             auto ss = ws.lock();
                             if (!ss) return;
                             self->on_resume_result(ss, std::move(r));
                         });
        }
    }

    void on_resume_result(const std::shared_ptr<ClientSession>& s, Result<json> r) {
        if (!r) return;  // link dropped again; next connect retries
        if (!r->value("ok", false)) {
            LogRecord(LogLevel::Warning, "Sessions").session(s->id()).op("resume").result(r->value("message", ""));
            const DeviceInfo d = s->device();
            sessions_.erase(s->handle());
            s->close("resume rejected: " + r->value("message", std::string("unknown")));
            eng_.unregister_session(s);
            // Restore when technically possible: re-attach the same device.
            eng_.attach(entry_.name, d.id, AccessMode::Exclusive, SessionPriority::High,
                        [name = entry_.name, id = d.id](Result<std::string> res) {
                            if (res) log::info("Sessions", "device " + id + " re-attached after session expiry");
                            else log::warn("Sessions", "re-attach of " + id + " failed: " + res.error().describe());
                        });
            return;
        }
        auto desc = descriptor_set_from_json(r->value("descriptors", json::object()));
        auto info = device_info_from_json(r->value("device", json::object()));
        UsbSpeed speed = UsbSpeed::High;
        parse_speed(r->value("speed", "high"), speed);
        if (desc && info && info->identity.descriptor_hash != s->device().identity.descriptor_hash) {
            // Device changed mode while we were away: replug with fresh descriptors.
            s->device_reenumerated(*desc, speed, *info);
        }
        s->link_up(conn_);
        if (r->value("state", "") == "Reconnecting") s->device_removed();
        LogRecord(LogLevel::Info, "Sessions").session(s->id()).op("resume").result("ok").msg("session resumed");
        eng_.publish_event("session", s->to_json());
    }

    void refresh_devices() {
        send_request(MsgType::DeviceListRequest, json::object(), std::chrono::seconds(10),
                     [self = shared_from_this()](Result<json> r) {
                         if (!r) return;
                         std::vector<DeviceInfo> list;
                         for (const auto& d : r->value("devices", json::array())) {
                             if (auto di = device_info_from_json(d)) list.push_back(std::move(*di));
                         }
                         {
                             std::lock_guard lk(self->snap_m_);
                             self->devices_ = std::move(list);
                         }
                         self->eng_.publish_event("devices", json{{"server", self->entry_.name}});
                         self->auto_attach();
                     });
    }

    void auto_attach() {
        if (entry_.auto_attach.empty()) return;
        for (const auto& d : devices()) {
            if (d.state != DeviceShareState::Shared) continue;
            const std::string vp = format_vid_pid(d.identity.vid, d.identity.pid);
            bool match = false;
            for (const auto& p : entry_.auto_attach)
                match |= TokenStore::glob_match(p, d.id) || TokenStore::glob_match(p, vp) ||
                         TokenStore::glob_match(p, d.identity.port_path);
            if (!match || attaching_.count(d.id)) continue;
            attaching_.insert(d.id);
            eng_.attach(entry_.name, d.id, AccessMode::Exclusive, SessionPriority::High,
                        [self = shared_from_this(), id = d.id](Result<std::string>) {
                            asio::post(self->strand_, [self, id] { self->attaching_.erase(id); });
                        });
        }
    }

    // ---------------------------------------------------------------- frames
    void send_request(MsgType type, json body, std::chrono::milliseconds timeout, Reply cb) {
        const std::uint32_t corr = next_corr_++;
        if (next_corr_ == 0) next_corr_ = 1;
        auto t = std::make_shared<asio::steady_timer>(strand_, timeout);
        t->async_wait([self = shared_from_this(), corr](std::error_code ec) {
            if (ec) return;
            auto it = self->pending_.find(corr);
            if (it == self->pending_.end()) return;
            auto p = std::move(it->second);
            self->pending_.erase(it);
            p.cb(Error{ErrorCode::Timeout, "request timed out"});
        });
        pending_[corr] = Pending{std::move(cb), t};
        conn_->send(encode_json(type, body, corr), true);
    }

    void on_frame(InFrame&& f) {
        switch (f.header.type) {
            case MsgType::UsbComplete: {
                UsbCompleteMsg m;
                if (!decode_complete(f.bytes(), m)) return fail_connection(Error{ErrorCode::MalformedPayload, "USB_COMPLETE"});
                if (auto it = sessions_.find(m.device); it != sessions_.end()) it->second->on_complete(m);
                return;
            }
            case MsgType::UsbOpResult: {
                UsbOpResultMsg m;
                if (!decode_op_result(f.bytes(), m)) return;
                if (auto it = sessions_.find(m.device); it != sessions_.end()) it->second->on_op_result(m);
                return;
            }
            default:
                break;
        }
        // control plane
        if (f.header.flags & frame_flags::kResponse && f.header.correlation) {
            auto it = pending_.find(f.header.correlation);
            if (it == pending_.end()) return;
            auto p = std::move(it->second);
            pending_.erase(it);
            p.timer->cancel();
            if (f.header.type == MsgType::Error) {
                auto j = decode_json(f);
                p.cb(Error{j ? static_cast<ErrorCode>(j->value("code", 1)) : ErrorCode::Unknown,
                           j ? j->value("message", "") : std::string("error")});
                return;
            }
            p.cb(decode_json(f));
            return;
        }
        auto j = decode_json(f);
        switch (f.header.type) {
            case MsgType::DeviceEvent:
                if (j) on_device_event(*j);
                return;
            case MsgType::DeviceReenumerated:
                if (j) on_reenumerated(*j);
                return;
            case MsgType::Error:
                log::warn("Network", entry_.name + " error: " + (j ? j->value("message", "") : std::string("?")));
                return;
            case MsgType::Goodbye:
                log::info("Network", entry_.name + " says goodbye: " + (j ? j->value("reason", "") : std::string()));
                return;
            default:
                return;
        }
    }

    void on_device_event(const json& j) {
        const std::string ev = j.value("event", "");
        if (ev == "session-closed") {
            auto it = sessions_.find(j.value("handle", 0u));
            if (it != sessions_.end()) {
                auto s = it->second;
                sessions_.erase(it);
                s->close("closed by server: " + j.value("reason", std::string()));
                eng_.unregister_session(s);
            }
            return;
        }
        if (ev == "removed" && j.value("reconnecting", false)) {
            if (auto it = sessions_.find(j.value("handle", 0u)); it != sessions_.end()) {
                it->second->device_removed();
                eng_.publish_event("session", it->second->to_json());
            }
        }
        // update the device list cache
        if (auto d = device_info_from_json(j.value("device", json::object()))) {
            std::lock_guard lk(snap_m_);
            auto it = std::find_if(devices_.begin(), devices_.end(), [&](const DeviceInfo& x) { return x.id == d->id; });
            if (ev == "removed") {
                if (it != devices_.end()) devices_.erase(it);
            } else if (it != devices_.end()) {
                *it = *d;
            } else {
                devices_.push_back(*d);
            }
        }
        eng_.publish_event("devices", json{{"server", entry_.name}, {"event", ev}});
        if (ev == "arrived" || ev == "changed") auto_attach();
    }

    void on_reenumerated(const json& j) {
        auto it = sessions_.find(j.value("handle", 0u));
        if (it == sessions_.end()) return;
        auto desc = descriptor_set_from_json(j.value("descriptors", json::object()));
        auto info = device_info_from_json(j.value("device", json::object()));
        if (!desc || !info) {
            log::error("Sessions", "invalid DEVICE_REENUMERATED payload");
            return;
        }
        UsbSpeed speed = UsbSpeed::High;
        parse_speed(j.value("speed", "high"), speed);
        it->second->device_reenumerated(*desc, speed, *info);
        eng_.publish_event("session", it->second->to_json());
    }

    struct Pending {
        Reply cb;
        std::shared_ptr<asio::steady_timer> timer;
    };

    ClientEngine& eng_;
    ServerEntry entry_;
    net::Strand strand_;
    asio::steady_timer timer_;
    std::shared_ptr<LinkMetrics> metrics_;
    std::shared_ptr<net::FrameConnection> conn_;
    LinkState state_ = LinkState::Disconnected;
    bool user_disconnect_ = false;
    bool had_connection_ = false;
    std::uint32_t attempt_ = 0;
    std::uint32_t next_corr_ = 1;
    std::map<std::uint32_t, Pending> pending_;
    std::map<std::uint32_t, std::shared_ptr<ClientSession>> sessions_;  // by server handle
    std::set<std::string> attaching_;

    mutable std::mutex snap_m_;
    LinkState pub_state_ = LinkState::Disconnected;
    std::string last_error_;
    std::string server_name_, server_version_;
    std::vector<DeviceInfo> devices_;
    std::size_t session_count_ = 0;

    friend class ClientEngine;
};

// =====================================================================
// ClientEngine
// =====================================================================

ClientEngine::ClientEngine(ClientConfig cfg, std::unique_ptr<IVirtualHci> vhci, TokenProvider tokens)
    : cfg_(std::move(cfg)), vhci_(std::move(vhci)), tokens_(std::move(tokens)) {
    if (cfg_.client_id.empty()) cfg_.client_id = crypto::uuid4();
    if (cfg_.client_name.empty()) cfg_.client_name = host_name();
}

ClientEngine::~ClientEngine() { stop(); }

void ClientEngine::set_event_handler(EventFn fn) {
    std::lock_guard lk(ev_m_);
    on_event_ = std::move(fn);
}

void ClientEngine::publish_event(const std::string& kind, const json& body) {
    EventFn fn;
    {
        std::lock_guard lk(ev_m_);
        fn = on_event_;
    }
    if (fn) fn(kind, body);
}

Status ClientEngine::start() {
    if (running_) return {};
    if (auto st = validate(cfg_); !st) return st;
    rt_ = std::make_unique<net::IoRuntime>(effective_worker_threads(cfg_.worker_threads), "pf-client-io");
    running_ = true;
    if (vhci_) {
        vhci_->set_sink([this](const VhciRequest& r) { on_vhci_request(r); });
        auto st = vhci_->open();
        vhci_open_ = st.ok();
        if (!st) {
            std::lock_guard lk(m_);
            vhci_error_ = st.error().describe();
            // Not fatal: the client can still browse servers; attach reports the error.
            LogRecord(LogLevel::Error, "VirtualUSB").op("open").result(vhci_error_).msg(
                "virtual USB controller unavailable - remote devices cannot be attached");
        }
    } else {
        std::lock_guard lk(m_);
        vhci_error_ = "NOT IMPLEMENTED on this platform: no virtual USB controller driver";
    }
    for (const auto& e : cfg_.servers) {
        auto l = std::make_shared<ServerLink>(*this, e);
        {
            std::lock_guard lk(m_);
            links_[e.name] = l;
        }
        if (e.auto_connect) l->connect();
    }
    metrics_timer_ = std::make_unique<asio::steady_timer>(rt_->context());
    sample_metrics();
    LogRecord(LogLevel::Info, "Client").op("start").msg("ParsFlash USB Client " + std::string(PF_VERSION_STRING) +
                                                        " started as " + cfg_.client_name);
    return {};
}

void ClientEngine::stop() {
    if (!running_.exchange(false)) return;
    std::vector<std::shared_ptr<ServerLink>> links;
    std::vector<std::shared_ptr<ClientSession>> sessions;
    {
        std::lock_guard lk(m_);
        for (auto& [n, l] : links_) links.push_back(l);
        for (auto& [id, s] : sessions_) sessions.push_back(s);
    }
    for (auto& s : sessions) s->close("client shutting down");
    for (auto& l : links) l->disconnect();
    if (metrics_timer_) metrics_timer_->cancel();
    if (vhci_) vhci_->close();
    rt_->stop();
    std::lock_guard lk(m_);
    sessions_.clear();
    by_cookie_.clear();
    links_.clear();
}

std::shared_ptr<ServerLink> ClientEngine::link(const std::string& name) const {
    std::lock_guard lk(m_);
    auto it = links_.find(name);
    return it == links_.end() ? nullptr : it->second;
}

Status ClientEngine::add_server(const ServerEntry& e) {
    if (e.host.empty() || e.port == 0) return {ErrorCode::InvalidArgument, "host/port"};
    auto l = std::make_shared<ServerLink>(*this, e);
    {
        std::lock_guard lk(m_);
        if (links_.count(e.name)) return {ErrorCode::AlreadyExists, e.name};
        links_[e.name] = l;
        cfg_.servers.push_back(e);
    }
    if (running_ && e.auto_connect) l->connect();
    return {};
}

Status ClientEngine::remove_server(const std::string& name) {
    auto l = link(name);
    if (!l) return {ErrorCode::NotFound, name};
    l->disconnect();
    std::lock_guard lk(m_);
    links_.erase(name);
    cfg_.servers.erase(std::remove_if(cfg_.servers.begin(), cfg_.servers.end(),
                                      [&](const ServerEntry& e) { return e.name == name; }),
                       cfg_.servers.end());
    return {};
}

Status ClientEngine::connect(const std::string& name) {
    auto l = link(name);
    if (!l) return {ErrorCode::NotFound, name};
    l->connect();
    return {};
}

Status ClientEngine::disconnect(const std::string& name) {
    auto l = link(name);
    if (!l) return {ErrorCode::NotFound, name};
    l->disconnect();
    return {};
}

void ClientEngine::debug_drop_connection(const std::string& name) {
    if (auto l = link(name)) l->drop_for_test();
}

std::vector<ServerSnapshot> ClientEngine::servers() const {
    std::vector<std::shared_ptr<ServerLink>> links;
    {
        std::lock_guard lk(m_);
        for (auto& [n, l] : links_) links.push_back(l);
    }
    std::vector<ServerSnapshot> out;
    for (auto& l : links) {
        auto s = l->snapshot();
        std::lock_guard lk(m_);
        s.sessions = 0;
        for (auto& [id, ss] : sessions_) s.sessions += ss->server() == s.name;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<DeviceInfo> ClientEngine::devices(const std::string& server) const {
    auto l = link(server);
    return l ? l->devices() : std::vector<DeviceInfo>{};
}

ServerEntry ClientEngine::server_entry(const std::string& name) const {
    auto l = link(name);
    return l ? l->entry() : ServerEntry{};
}

// ------------------------------------------------------------ sessions

void ClientEngine::attach(const std::string& server, const std::string& device_id, AccessMode access,
                          SessionPriority prio, AttachCallback cb) {
    auto l = link(server);
    if (!l) return cb(Error{ErrorCode::NotFound, "unknown server " + server});
    if (!vhci_open_ && access == AccessMode::Exclusive)
        return cb(Error{ErrorCode::DriverNotInstalled, vhci_error()});
    l->request(MsgType::DeviceAttach,
               json{{"device_id", device_id},
                    {"access", std::string(to_string(access))},
                    {"priority", std::string(to_string(prio))},
                    {"mobile_repair", cfg_.mobile_repair_mode}},
               std::chrono::seconds(20), [this, l, server, access, prio, cb](Result<json> r) {
                   if (!r) return cb(r.error());
                   if (!r->value("ok", false))
                       return cb(Error{static_cast<ErrorCode>(r->value("code", 1)), r->value("message", "")});
                   auto desc = descriptor_set_from_json(r->value("descriptors", json::object()));
                   auto info = device_info_from_json(r->value("device", json::object()));
                   if (!desc || !info) return cb(Error{ErrorCode::MalformedPayload, "attach result"});
                   SessionParams p;
                   p.session_id = r->value("session_id", "");
                   p.resume_token = r->value("resume_token", "");
                   p.handle = r->value("handle", 0u);
                   p.server = server;
                   p.device = *info;
                   p.descriptors = *desc;
                   parse_speed(r->value("speed", "high"), p.speed);
                   p.access = access;
                   p.priority = prio;
                   auto s = std::make_shared<ClientSession>(p, *vhci_, next_cookie_.fetch_add(1));
                   s->state_machine().transition(SessionState::Attaching, "attach accepted by server");
                   register_session(s);
                   l->attach_session(s);
                   if (access == AccessMode::Exclusive) {
                       if (auto st = s->plug(); !st) {
                           s->state_machine().transition(SessionState::Failed, st.error().describe());
                           l->forget_session(s->handle());
                           unregister_session(s);
                           l->request(MsgType::DeviceDetach, json{{"handle", p.handle}}, std::chrono::seconds(5),
                                      [](Result<json>) {});
                           return cb(st.error());
                       }
                   }
                   asio::post(l->strand(), [l, s, cb, this] {
                       s->link_up(l->connection());
                       publish_event("session", s->to_json());
                       cb(s->id());
                   });
               });
}

Result<std::string> ClientEngine::attach_sync(const std::string& server, const std::string& device_id,
                                              AccessMode access, std::optional<SessionPriority> prio,
                                              std::chrono::milliseconds timeout) {
    SessionPriority pr = SessionPriority::High;
    if (prio) pr = *prio;
    else parse_priority(cfg_.default_priority, pr);
    auto prom = std::make_shared<std::promise<Result<std::string>>>();
    auto fut = prom->get_future();
    attach(server, device_id, access, pr, [prom](Result<std::string> r) { prom->set_value(std::move(r)); });
    if (fut.wait_for(timeout) != std::future_status::ready) return Error{ErrorCode::Timeout, "attach timed out"};
    return fut.get();
}

Status ClientEngine::detach(const std::string& session_id) {
    auto s = session(session_id);
    if (!s) return {ErrorCode::NotFound, "session"};
    auto l = link(s->server());
    s->close("detached by user");
    unregister_session(s);
    if (l) {
        l->forget_session(s->handle());
        l->request(MsgType::DeviceDetach, json{{"handle", s->handle()}}, std::chrono::seconds(5), [](Result<json>) {});
    }
    return {};
}

void ClientEngine::reset_device(const std::string& session_id) {
    auto s = session(session_id);
    if (!s) return;
    auto l = link(s->server());
    if (!l) return;
    asio::post(l->strand(), [l, s] {
        if (auto c = l->connection()) c->send(encode_reset(UsbResetMsg{0, s->handle(), ResetKind::PortReset}), true);
    });
}

void ClientEngine::register_session(const std::shared_ptr<ClientSession>& s) {
    std::lock_guard lk(m_);
    sessions_[s->id()] = s;
    by_cookie_[s->cookie()] = s;
}

void ClientEngine::unregister_session(const std::shared_ptr<ClientSession>& s) {
    {
        std::lock_guard lk(m_);
        sessions_.erase(s->id());
        by_cookie_.erase(s->cookie());
    }
    publish_event("session", s->to_json());
}

std::shared_ptr<ClientSession> ClientEngine::session(const std::string& id) const {
    std::lock_guard lk(m_);
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<ClientSession>> ClientEngine::sessions() const {
    std::vector<std::shared_ptr<ClientSession>> out;
    std::lock_guard lk(m_);
    for (auto& [id, s] : sessions_) out.push_back(s);
    return out;
}

void ClientEngine::on_vhci_request(const VhciRequest& r) {
    std::shared_ptr<ClientSession> s;
    {
        std::lock_guard lk(m_);
        auto it = by_cookie_.find(r.cookie);
        if (it != by_cookie_.end()) s = it->second;
    }
    if (s) {
        s->on_vhci_request(r);
        return;
    }
    // Orphan record (session already gone): fail it so the URB does not hang.
    if (r.kind != VhciRecordKind::Cancel && r.kind != VhciRecordKind::DeviceRemoved) {
        VhciCompletion c;
        c.is_op_result = r.kind != VhciRecordKind::Submit;
        c.device_handle = r.device_handle;
        c.cookie = r.cookie;
        c.request_id = r.request_id;
        c.status = UsbStatus::DeviceGone;
        (void)vhci_->complete(c);
    }
}

VhciInfo ClientEngine::vhci_info() const { return vhci_ ? vhci_->info() : VhciInfo{}; }

std::string ClientEngine::vhci_error() const {
    std::lock_guard lk(m_);
    return vhci_error_;
}

void ClientEngine::sample_metrics() {
    if (!running_) return;
    const std::uint64_t now = monotonic_ns();
    for (auto& s : sessions()) s->metrics().sample(now);
    std::vector<std::shared_ptr<ServerLink>> links;
    {
        std::lock_guard lk(m_);
        for (auto& [n, l] : links_) links.push_back(l);
        cpu_percent_ = cpu_.sample();
    }
    for (auto& l : links) l->metrics().sample(now);
    metrics_timer_->expires_after(std::chrono::seconds(1));
    metrics_timer_->async_wait([this](std::error_code ec) {
        if (!ec) sample_metrics();
    });
}

json ClientEngine::status_json() const {
    json servers = json::array();
    for (const auto& s : this->servers()) {
        servers.push_back({{"name", s.name},
                           {"host", s.host},
                           {"port", s.port},
                           {"tls", s.tls},
                           {"state", std::string(to_string(s.state))},
                           {"error", s.last_error},
                           {"fingerprint", s.fingerprint},
                           {"server_name", s.server_name},
                           {"latency_us", s.rtt_us},
                           {"upload_bps", s.tx_bps},
                           {"download_bps", s.rx_bps},
                           {"packet_loss", s.packet_loss},
                           {"reconnects", s.reconnects},
                           {"devices", s.devices},
                           {"sessions", s.sessions}});
    }
    const auto usage = sample_process_usage();
    const VhciInfo vi = vhci_info();
    double cpu;
    {
        std::lock_guard lk(m_);
        cpu = cpu_percent_;
    }
    return {{"product", "ParsFlash USB Redirector"},
            {"component", "client"},
            {"version", PF_VERSION_STRING},
            {"client_name", cfg_.client_name},
            {"client_id", cfg_.client_id},
            {"virtual_usb",
             {{"available", vhci_open_.load()},
              {"error", vhci_error()},
              {"driver_version", vi.driver_version},
              {"test_double", vi.is_test_double}}},
            {"servers", servers},
            {"sessions", sessions().size()},
            {"mobile_repair_mode", cfg_.mobile_repair_mode},
            {"cpu_percent", cpu},
            {"ram_bytes", usage.rss_bytes}};
}

json ClientEngine::metrics_json() const {
    json arr = json::array();
    for (auto& s : sessions()) arr.push_back(s->to_json());
    json links = json::array();
    std::vector<std::shared_ptr<ServerLink>> ls;
    {
        std::lock_guard lk(m_);
        for (auto& [n, l] : links_) ls.push_back(l);
    }
    for (auto& l : ls) {
        auto j = to_json(l->metrics());
        j["server"] = l->snapshot().name;
        links.push_back(j);
    }
    return {{"status", status_json()}, {"sessions", arr}, {"links", links}};
}

}  // namespace pf::client
