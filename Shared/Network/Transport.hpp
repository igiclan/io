// Byte-stream transports (TCP, TLS 1.3 over TCP) built on Asio.
// On Windows Asio drives sockets through an I/O completion port (overlapped
// WSARecv/WSASend, AcceptEx, ConnectEx); on Linux through epoll.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include "Common/ByteIo.hpp"
#include "Common/Config.hpp"
#include "Common/Error.hpp"

namespace pf::net {

using tcp = asio::ip::tcp;
using Strand = asio::strand<asio::io_context::executor_type>;

// Owns an io_context and its worker threads.
class IoRuntime {
public:
    explicit IoRuntime(std::uint32_t threads, const char* name = "pf-io");
    ~IoRuntime();
    IoRuntime(const IoRuntime&) = delete;
    IoRuntime& operator=(const IoRuntime&) = delete;

    asio::io_context& context() noexcept { return ctx_; }
    Strand make_strand() { return asio::make_strand(ctx_); }
    void stop();
    [[nodiscard]] std::size_t thread_count() const noexcept { return threads_.size(); }

private:
    asio::io_context ctx_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stopped_{false};
};

class Transport {
public:
    using IoHandler = std::function<void(std::error_code, std::size_t)>;
    virtual ~Transport() = default;
    // Handlers are invoked on the transport's strand.
    virtual void async_read_some(MutableByteSpan buf, IoHandler h) = 0;
    virtual void async_write(const std::vector<asio::const_buffer>& bufs, IoHandler h) = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool is_secure() const noexcept = 0;
    [[nodiscard]] virtual std::string remote_address() const = 0;
    [[nodiscard]] virtual std::string peer_fingerprint() const { return {}; }
    virtual Strand& strand() noexcept = 0;
};

using TransportPtr = std::unique_ptr<Transport>;

// TLS contexts. TLS 1.3 only.
Result<std::shared_ptr<asio::ssl::context>> make_server_tls_context(const std::string& cert_pem_file,
                                                                    const std::string& key_pem_file);
std::shared_ptr<asio::ssl::context> make_client_tls_context();

void apply_socket_options(tcp::socket& s, const NetworkTuning& t, const KeepaliveConfig& k);
Error error_from_asio(const std::error_code& ec, std::string_view what);

// --------------------------------------------------------------- listener

struct ListenerOptions {
    std::string address = "0.0.0.0";
    std::uint16_t port = 39000;
    std::shared_ptr<asio::ssl::context> tls;  // null = plaintext
    NetworkTuning tuning;
    KeepaliveConfig keepalive;
    std::uint32_t handshake_timeout_ms = 10000;
};

// Accepts connections; performs the TLS handshake (with timeout) before handing the
// transport to on_accept. Each accepted transport gets its own strand.
class Listener : public std::enable_shared_from_this<Listener> {
public:
    using AcceptHandler = std::function<void(TransportPtr)>;
    Listener(IoRuntime& rt, ListenerOptions opts);
    Status start(AcceptHandler on_accept);
    void stop();
    [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }

private:
    void do_accept();
    IoRuntime& rt_;
    ListenerOptions opts_;
    tcp::acceptor acceptor_;
    AcceptHandler on_accept_;
    std::uint16_t bound_port_ = 0;
    std::atomic<bool> stopped_{false};
};

// --------------------------------------------------------------- connector

struct ConnectOptions {
    std::string host;
    std::uint16_t port = 39000;
    bool tls = true;
    std::string expected_fingerprint;  // empty = accept any (trust on first use; caller pins)
    NetworkTuning tuning;
    KeepaliveConfig keepalive;
    std::uint32_t timeout_ms = 10000;
};

using ConnectHandler = std::function<void(Result<TransportPtr>)>;
void async_connect(IoRuntime& rt, ConnectOptions opts, ConnectHandler h);

}  // namespace pf::net
