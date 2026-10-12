#include "API/RestApi.hpp"

#include <cctype>
#include <charconv>
#include <nlohmann/json.hpp>
#include <vector>

#include "Common/Platform.hpp"
#include "Core/ServerEngine.hpp"
#include "Logging/Logger.hpp"

namespace pf::server {

using nlohmann::json;

namespace api {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Calls f(lowercase name, trimmed value) for every header line after the request
// line. f returns false to stop early.
template <typename F>
void for_each_header(std::string_view head, F&& f) {
    std::size_t pos = head.find("\r\n");
    while (pos != std::string_view::npos) {
        pos += 2;
        const std::size_t next = head.find("\r\n", pos);
        const std::string_view line =
            head.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos && !f(lower(trim(line.substr(0, colon))), trim(line.substr(colon + 1))))
            return;
        pos = next;
    }
}

const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 501: return "Not Implemented";
        default: return "Internal Server Error";
    }
}

Response json_response(int status, const json& j) { return Response{status, j.dump()}; }

int http_status_for(ErrorCode c) {
    switch (c) {
        case ErrorCode::NotFound:
        case ErrorCode::DeviceNotFound: return 404;
        case ErrorCode::AccessDenied: return 403;
        case ErrorCode::InvalidState:
        case ErrorCode::DeviceBusy: return 409;
        case ErrorCode::InvalidArgument:
        case ErrorCode::MalformedPayload: return 400;
        case ErrorCode::NotImplemented:
        case ErrorCode::NotSupported: return 501;
        default: return 500;
    }
}

Response from_status(const Status& st) {
    if (st) return json_response(200, json{{"ok", true}});
    return error_response(http_status_for(st.error().code), st.error().code, st.error().message);
}

json devices_json(ServerEngine& eng) {
    json arr = json::array();
    for (const auto& d : eng.devices()) arr.push_back(to_json(d));
    return arr;
}

json clients_json(ServerEngine& eng) {
    json arr = json::array();
    for (const auto& c : eng.clients()) {
        arr.push_back(json{{"connection_id", c.connection_id},
                           {"client_id", c.client_id},
                           {"name", c.name},
                           {"address", c.address},
                           {"authenticated", c.authenticated},
                           {"secure", c.secure},
                           {"token", c.token_name},
                           {"connected", iso8601_utc(c.connected_ms)},
                           {"latency_us", c.rtt_us},
                           {"upload_bps", c.tx_bps},
                           {"download_bps", c.rx_bps},
                           {"sessions", c.sessions}});
    }
    return arr;
}

Response device_action(ServerEngine& eng, const TokenIdentity& who, const std::string& id,
                       const std::string& action, const std::string& body) {
    if (action == "attach") {
        // Attach is initiated by the client (DEVICE_ATTACH), which owns the data path.
        return error_response(501, ErrorCode::NotImplemented,
                              "attach is started by a ParsFlash USB Client; the management API cannot open sessions yet");
    }
    Status st;
    if (action == "share") {
        bool shared = true;
        if (!body.empty()) {
            auto j = json::parse(body, nullptr, false);
            if (j.is_discarded() || !j.is_object())
                return error_response(400, ErrorCode::InvalidArgument, "body must be a JSON object");
            if (j.contains("shared")) {
                if (!j["shared"].is_boolean())
                    return error_response(400, ErrorCode::InvalidArgument, "'shared' must be true or false");
                shared = j["shared"].get<bool>();
            }
        }
        st = eng.share(id, shared);
    } else if (action == "reset") {
        st = eng.reset_device(id);
        if (st) {
            LogRecord(LogLevel::Info, "API").client(who.name).device(id).op("reset").result("accepted");
            return json_response(202, json{{"ok", true}, {"status", "accepted"}});
        }
    } else {  // detach
        st = eng.detach_device(id, "detached via management API by " + who.name);
    }
    LogRecord(LogLevel::Info, "API")
        .client(who.name)
        .device(id)
        .op(action)
        .result(st ? "ok" : st.error().describe());
    return from_status(st);
}

}  // namespace

Response error_response(int status, ErrorCode code, std::string_view message) {
    return Response{status, json{{"error", json{{"code", std::string(to_string(code))},
                                                {"message", std::string(message)}}}}
                                .dump()};
}

Framing frame_request(std::string_view buf, std::size_t& total, std::string& why) {
    total = 0;
    const auto end = buf.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        if (buf.size() > kMaxHeadBytes) {
            why = "request headers too large";
            return Framing::TooLarge;
        }
        return Framing::Incomplete;
    }
    if (end + 4 > kMaxHeadBytes) {
        why = "request headers too large";
        return Framing::TooLarge;
    }
    std::size_t content_length = 0;
    bool has_length = false;
    bool bad = false;
    for_each_header(buf.substr(0, end), [&](const std::string& name, std::string_view value) {
        if (name == "transfer-encoding") {
            why = "Transfer-Encoding is not supported; send Content-Length";
            bad = true;
            return false;
        }
        if (name == "content-length") {
            if (has_length) {
                why = "duplicate Content-Length";
                bad = true;
                return false;
            }
            const char* first = value.data();
            const char* last = value.data() + value.size();
            const auto [p, ec] = std::from_chars(first, last, content_length);
            if (ec != std::errc{} || p != last) {
                why = "invalid Content-Length";
                bad = true;
                return false;
            }
            has_length = true;
        }
        return true;
    });
    if (bad) return Framing::Malformed;
    if (content_length > kMaxBodyBytes) {
        why = "request body too large";
        return Framing::TooLarge;
    }
    total = end + 4 + content_length;
    return buf.size() < total ? Framing::Incomplete : Framing::Complete;
}

Result<Request> parse_request(std::string_view raw) {
    const auto end = raw.find("\r\n\r\n");
    if (end == std::string_view::npos) return Error{ErrorCode::MalformedPayload, "incomplete request"};
    const std::string_view head = raw.substr(0, end);
    const std::string_view line = head.substr(0, head.find("\r\n"));
    const auto sp1 = line.find(' ');
    const auto sp2 = line.rfind(' ');
    if (sp1 == std::string_view::npos || sp2 == sp1) return Error{ErrorCode::MalformedPayload, "bad request line"};
    const std::string_view version = line.substr(sp2 + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0")
        return Error{ErrorCode::MalformedPayload, "unsupported HTTP version"};
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (target.empty() || target[0] != '/') return Error{ErrorCode::MalformedPayload, "target must be an absolute path"};
    if (const auto q = target.find('?'); q != std::string_view::npos) target = target.substr(0, q);

    Request r;
    r.method = std::string(line.substr(0, sp1));
    r.path = std::string(target);
    for_each_header(head, [&](const std::string& name, std::string_view value) {
        if (name == "authorization" && value.size() > 7 && lower(value.substr(0, 7)) == "bearer ")
            r.bearer_token = std::string(trim(value.substr(7)));
        return true;
    });
    r.body = std::string(raw.substr(end + 4));
    return r;
}

Response handle(ServerEngine& eng, const Request& req) {
    if (req.path != "/api" && req.path.rfind("/api/", 0) != 0)
        return error_response(404, ErrorCode::NotFound, "no such endpoint");
    if (req.bearer_token.empty())
        return error_response(401, ErrorCode::AuthRequired, "Authorization: Bearer <token> required");
    auto who = eng.tokens().verify(req.bearer_token);
    if (!who) return error_response(401, ErrorCode::AuthFailed, "invalid token");

    std::vector<std::string> seg;
    {
        const std::string_view rest = std::string_view(req.path).substr(4);  // after "/api"
        std::size_t pos = 0;
        while (pos < rest.size()) {
            if (rest[pos] == '/') {
                ++pos;
                continue;
            }
            const auto next = rest.find('/', pos);
            seg.emplace_back(rest.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
            pos = next == std::string_view::npos ? rest.size() : next;
        }
    }
    const bool is_get = req.method == "GET";
    const bool is_post = req.method == "POST";
    const auto method_not_allowed = [] { return error_response(405, ErrorCode::InvalidArgument, "method not allowed"); };

    if (seg.size() == 1) {
        const std::string& name = seg[0];
        if (name == "status" || name == "metrics" || name == "sessions" || name == "clients") {
            if (!is_get) return method_not_allowed();
            if (name == "status") return json_response(200, eng.status_json());
            if (name == "metrics") return json_response(200, eng.metrics_json());
            if (name == "sessions") return json_response(200, eng.sessions_json());
            return json_response(200, clients_json(eng));
        }
        if (name == "devices") {
            if (!is_get) return method_not_allowed();
            return json_response(200, devices_json(eng));
        }
    }
    if (seg.size() >= 2 && seg[0] == "devices") {
        const std::string& id = seg[1];
        if (seg.size() == 2) {
            if (!is_get) return method_not_allowed();
            for (const auto& d : eng.devices())
                if (d.id == id) return json_response(200, to_json(d));
            return error_response(404, ErrorCode::DeviceNotFound, "no such device");
        }
        if (seg.size() == 3) {
            const std::string& action = seg[2];
            if (action == "share" || action == "reset" || action == "detach" || action == "attach") {
                if (!is_post) return method_not_allowed();
                if (!who->admin)
                    return error_response(403, ErrorCode::AccessDenied, "this endpoint requires an administrator token");
                return device_action(eng, *who, id, action, req.body);
            }
        }
    }
    return error_response(404, ErrorCode::NotFound, "no such endpoint");
}

std::string serialize(const Response& r) {
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason_phrase(r.status) + "\r\n";
    out += "Content-Type: application/json; charset=utf-8\r\n";
    out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    out += "Connection: close\r\n\r\n";
    out += r.body;
    return out;
}

}  // namespace api

// ------------------------------------------------------------------ connection

namespace {

class ApiConnection : public std::enable_shared_from_this<ApiConnection> {
public:
    ApiConnection(ServerEngine& e, net::TransportPtr t)
        : eng_(e), t_(std::move(t)), timer_(t_->strand()), buf_(api::kMaxRequestBytes) {}

    void start() {
        timer_.expires_after(kIdleTimeout);
        timer_.async_wait([self = shared_from_this()](std::error_code ec) {
            if (!ec) self->t_->close();  // idle or slow client
        });
        read();
    }

private:
    static constexpr std::chrono::seconds kIdleTimeout{10};

    std::string_view view() const { return {reinterpret_cast<const char*>(buf_.data()), filled_}; }

    void read() {
        t_->async_read_some(MutableByteSpan(buf_.data() + filled_, buf_.size() - filled_),
                            [self = shared_from_this()](std::error_code ec, std::size_t n) { self->on_read(ec, n); });
    }

    void on_read(std::error_code ec, std::size_t n) {
        if (ec) return finish();
        filled_ += n;
        std::size_t total = 0;
        std::string why;
        switch (api::frame_request(view(), total, why)) {
            case api::Framing::Incomplete:
                if (filled_ >= buf_.size())
                    return reply(api::error_response(413, ErrorCode::ResourceExhausted, "request too large"));
                return read();
            case api::Framing::TooLarge:
                return reply(api::error_response(413, ErrorCode::ResourceExhausted, why));
            case api::Framing::Malformed:
                return reply(api::error_response(400, ErrorCode::MalformedPayload, why));
            case api::Framing::Complete:
                break;
        }
        auto req = api::parse_request(view().substr(0, total));
        if (!req) return reply(api::error_response(400, ErrorCode::MalformedPayload, req.error().message));
        reply(api::handle(eng_, *req));
    }

    void reply(const api::Response& r) {
        auto out = std::make_shared<std::string>(api::serialize(r));
        timer_.cancel();
        std::vector<asio::const_buffer> bufs{asio::buffer(*out)};
        t_->async_write(bufs, [self = shared_from_this(), out](std::error_code, std::size_t) { self->finish(); });
    }

    void finish() { t_->close(); }

    ServerEngine& eng_;
    net::TransportPtr t_;
    asio::steady_timer timer_;
    std::vector<std::uint8_t> buf_;
    std::size_t filled_ = 0;
};

}  // namespace

// ------------------------------------------------------------------ RestApi

RestApi::RestApi(ServerEngine& engine) : engine_(engine) {}

RestApi::~RestApi() { stop(); }

Status RestApi::start(net::IoRuntime& rt, const std::string& address, std::uint16_t port) {
    net::ListenerOptions o;
    o.address = address;
    o.port = port;
    ServerEngine* eng = &engine_;
    listener_ = std::make_shared<net::Listener>(rt, o);
    if (auto st = listener_->start([eng](net::TransportPtr t) {
            std::make_shared<ApiConnection>(*eng, std::move(t))->start();
        });
        !st) {
        listener_.reset();
        return st;
    }
    if (address != "127.0.0.1" && address != "::1" && address != "localhost")
        log::warn("API", "management API listens on " + address + " without TLS: bearer tokens travel in plaintext");
    log::info("API", "management API listening on " + address + ":" + std::to_string(listener_->port()));
    return {};
}

void RestApi::stop() {
    if (!listener_) return;
    listener_->stop();
    listener_.reset();
}

std::uint16_t RestApi::port() const noexcept { return listener_ ? listener_->port() : 0; }

}  // namespace pf::server
