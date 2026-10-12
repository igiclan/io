// ParsFlash USB Server management REST API.
//
// HTTP/1.1, one request per connection (Connection: close), JSON bodies.
// Authentication: "Authorization: Bearer <token>" with a token from the TokenStore.
// Any valid token may read; only tokens with the admin flag may change state.
//
// The API is plaintext. It binds to 127.0.0.1 by default and must only be exposed
// through a trusted channel (SSH tunnel, VPN): tokens are not encrypted on the wire.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "Common/Error.hpp"
#include "Network/Transport.hpp"

namespace pf::server {

class ServerEngine;

namespace api {

inline constexpr std::size_t kMaxHeadBytes = 16 * 1024;   // request line + headers + CRLFCRLF
inline constexpr std::size_t kMaxBodyBytes = 64 * 1024;
inline constexpr std::size_t kMaxRequestBytes = kMaxHeadBytes + kMaxBodyBytes;

struct Request {
    std::string method;
    std::string path;          // without query string
    std::string bearer_token;  // empty when no Authorization: Bearer header
    std::string body;
};

struct Response {
    int status = 200;
    std::string body;  // JSON
};

enum class Framing { Incomplete, Complete, TooLarge, Malformed };

// Decides whether `buf` already holds one complete request; `total` receives its
// length in bytes. `why` explains TooLarge/Malformed.
Framing frame_request(std::string_view buf, std::size_t& total, std::string& why);
// `raw` must be exactly one complete request as framed above.
Result<Request> parse_request(std::string_view raw);
// Authenticates and routes. Never throws; all engine errors become HTTP errors.
Response handle(ServerEngine& engine, const Request& req);
Response error_response(int status, ErrorCode code, std::string_view message);
std::string serialize(const Response& r);

}  // namespace api

class RestApi {
public:
    explicit RestApi(ServerEngine& engine);
    ~RestApi();
    RestApi(const RestApi&) = delete;
    RestApi& operator=(const RestApi&) = delete;

    Status start(net::IoRuntime& rt, const std::string& address, std::uint16_t port);
    void stop();
    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    ServerEngine& engine_;
    std::shared_ptr<net::Listener> listener_;
};

}  // namespace pf::server
