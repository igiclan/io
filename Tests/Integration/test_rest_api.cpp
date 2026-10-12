// Management REST API: real HTTP over loopback against a running ServerEngine.
#include <doctest/doctest.h>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "API/RestApi.hpp"
#include "Harness.hpp"

using namespace pf;
using namespace pf::test;
using namespace std::chrono_literals;
using nlohmann::json;

namespace {

struct HttpReply {
    int status = 0;
    std::string body;
};

// Sends raw bytes and reads until the server closes the connection.
HttpReply send_raw(std::uint16_t port, const std::string& request) {
    asio::io_context io;
    asio::ip::tcp::socket s(io);
    s.connect({asio::ip::make_address("127.0.0.1"), port});
    asio::write(s, asio::buffer(request));
    std::string raw;
    char buf[4096];
    for (;;) {
        asio::error_code ec;
        const std::size_t n = s.read_some(asio::buffer(buf), ec);
        if (ec) break;
        raw.append(buf, n);
    }
    HttpReply r;
    const auto sp = raw.find(' ');
    if (sp != std::string::npos && raw.size() >= sp + 4) r.status = std::stoi(raw.substr(sp + 1, 3));
    const auto split = raw.find("\r\n\r\n");
    if (split != std::string::npos) r.body = raw.substr(split + 4);
    return r;
}

HttpReply http(std::uint16_t port, const std::string& method, const std::string& path, const std::string& token = {},
               const std::string& body = {}) {
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    if (!token.empty()) req += "Authorization: Bearer " + token + "\r\n";
    if (method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    return send_raw(port, req);
}

}  // namespace

TEST_CASE("management API: authentication and routing") {
    HarnessOptions o;
    o.api = true;
    Harness h(o);
    const auto port = h.server->api_port();
    REQUIRE(port != 0);
    CHECK(http(port, "GET", "/api/status").status == 401);
    CHECK(http(port, "GET", "/api/status", "pf_00000000_" + std::string(64, '0')).status == 401);
    auto r = http(port, "GET", "/api/status", h.token);
    REQUIRE(r.status == 200);
    auto j = json::parse(r.body);
    CHECK(j["product"] == "ParsFlash USB Redirector");
    CHECK(j["online"] == true);
    CHECK(j["devices"] == 2);
    CHECK(http(port, "GET", "/nope").status == 404);
    CHECK(http(port, "GET", "/api/unknown", h.token).status == 404);
    CHECK(http(port, "POST", "/api/status", h.token).status == 405);
}

TEST_CASE("management API: devices, sharing and token roles") {
    HarnessOptions o;
    o.api = true;
    Harness h(o);
    const auto port = h.server->api_port();
    auto viewer = h.server->tokens().create("viewer", false);
    REQUIRE(viewer.ok());
    const std::string id = h.device_on_port(1);
    REQUIRE_FALSE(id.empty());
    REQUIRE_FALSE(h.device_on_port(2).empty());

    auto list = json::parse(http(port, "GET", "/api/devices", h.token).body);
    REQUIRE(list.is_array());
    CHECK(list.size() == 2);

    auto one = http(port, "GET", "/api/devices/" + id, h.token);
    REQUIRE(one.status == 200);
    CHECK(json::parse(one.body)["vid_pid"] == "18d1:4ee7");
    CHECK(http(port, "GET", "/api/devices/dnothere00000", h.token).status == 404);

    // a viewer may read but not change anything
    CHECK(http(port, "GET", "/api/devices", *viewer).status == 200);
    CHECK(http(port, "POST", "/api/devices/" + id + "/share", *viewer, R"({"shared":false})").status == 403);
    CHECK(http(port, "POST", "/api/devices/" + id + "/reset", *viewer).status == 403);

    // an administrator stops sharing, then shares again
    CHECK(http(port, "POST", "/api/devices/" + id + "/share", h.token, R"({"shared":false})").status == 200);
    auto unshared = json::parse(http(port, "GET", "/api/devices/" + id, h.token).body);
    CHECK(unshared["shared"] == false);
    CHECK(unshared["state"] == "available");
    CHECK(http(port, "POST", "/api/devices/" + id + "/share", h.token).status == 200);  // default: shared
    CHECK(json::parse(http(port, "GET", "/api/devices/" + id, h.token).body)["shared"] == true);

    CHECK(http(port, "POST", "/api/devices/" + id + "/share", h.token, "{broken").status == 400);
    CHECK(http(port, "POST", "/api/devices/" + id + "/share", h.token, R"({"shared":"yes"})").status == 400);
    CHECK(http(port, "GET", "/api/devices/" + id + "/share", h.token).status == 405);
    CHECK(http(port, "POST", "/api/devices/" + id + "/attach", h.token).status == 501);
    CHECK(http(port, "POST", "/api/devices/" + id + "/detach", h.token).status == 409);  // not attached
    CHECK(http(port, "POST", "/api/devices/" + id + "/reset", h.token).status == 202);
}

TEST_CASE("management API: clients and sessions reflect live state") {
    HarnessOptions o;
    o.api = true;
    Harness h(o);
    const auto port = h.server->api_port();
    auto& c = h.add_client("PC-01");
    REQUIRE(h.wait_connected(c));
    const std::string id = h.device_on_port(1);
    REQUIRE(c.attach_sync("bench", id).ok());

    auto clients = json::parse(http(port, "GET", "/api/clients", h.token).body);
    REQUIRE(clients.size() == 1);
    CHECK(clients[0]["name"] == "PC-01");
    CHECK(clients[0]["authenticated"] == true);
    CHECK(clients[0]["secure"] == true);

    auto sessions = json::parse(http(port, "GET", "/api/sessions", h.token).body);
    REQUIRE(sessions.size() == 1);
    CHECK(sessions[0]["client_name"] == "PC-01");
    CHECK(sessions[0]["state"] == "Active");
    CHECK(http(port, "GET", "/api/metrics", h.token).status == 200);

    // administrator forces the detach: session closed, device free again
    CHECK(http(port, "POST", "/api/devices/" + id + "/detach", h.token).status == 200);
    CHECK(json::parse(http(port, "GET", "/api/sessions", h.token).body).empty());
    CHECK(json::parse(http(port, "GET", "/api/devices/" + id, h.token).body)["state"] == "shared");
}

TEST_CASE("management API: malformed and oversized requests are refused") {
    HarnessOptions o;
    o.api = true;
    Harness h(o);
    const auto port = h.server->api_port();
    // declared body far beyond the limit: refused before the body is read
    CHECK(send_raw(port, "POST /api/devices/x/share HTTP/1.1\r\nAuthorization: Bearer " + h.token +
                             "\r\nContent-Length: 100000\r\n\r\n")
              .status == 413);
    CHECK(send_raw(port, "GET /api/status HTTP/1.1\r\nAuthorization: Bearer " + h.token +
                             "\r\nTransfer-Encoding: chunked\r\n\r\n")
              .status == 400);
    CHECK(send_raw(port, "GET /api/status HTTP/1.1\r\nAuthorization: Bearer " + h.token +
                             "\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n")
              .status == 400);
    CHECK(send_raw(port, "GARBAGE\r\n\r\n").status == 400);
    // the server keeps serving afterwards
    CHECK(http(port, "GET", "/api/status", h.token).status == 200);
}

TEST_CASE("management API request framing and parsing") {
    using namespace pf::server::api;
    std::size_t total = 0;
    std::string why;
    CHECK(frame_request("GET /api/status HTTP/1.1\r\n", total, why) == Framing::Incomplete);
    CHECK(frame_request("POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nab", total, why) == Framing::Incomplete);
    CHECK(frame_request("POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nabcde", total, why) == Framing::Complete);
    CHECK(total == 44);
    CHECK(frame_request("POST /x HTTP/1.1\r\nContent-Length: -1\r\n\r\n", total, why) == Framing::Malformed);
    CHECK(frame_request(std::string(kMaxHeadBytes + 1, 'a'), total, why) == Framing::TooLarge);

    auto req = parse_request("POST /api/devices/d1/share?x=1 HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer  tok \r\n"
                             "Content-Length: 2\r\n\r\n{}");
    REQUIRE(req.ok());
    CHECK(req->method == "POST");
    CHECK(req->path == "/api/devices/d1/share");
    CHECK(req->bearer_token == "tok");
    CHECK(req->body == "{}");
    CHECK_FALSE(parse_request("GET api HTTP/1.1\r\n\r\n").ok());
    CHECK_FALSE(parse_request("GET /api HTTP/2.0\r\n\r\n").ok());
}
