#include "Network/Transport.hpp"

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "Common/Platform.hpp"
#include "Crypto/Crypto.hpp"
#include "Logging/Logger.hpp"

namespace pf::net {

// ------------------------------------------------------------- IoRuntime

IoRuntime::IoRuntime(std::uint32_t threads, const char* name)
    : ctx_(static_cast<int>(threads == 0 ? 1 : threads)), guard_(asio::make_work_guard(ctx_)) {
    if (threads == 0) threads = 1;
    for (std::uint32_t i = 0; i < threads; ++i) {
        threads_.emplace_back([this, name] {
            set_current_thread_name(name);
            for (;;) {
                try {
                    ctx_.run();
                    break;
                } catch (const std::exception& e) {
                    // A handler threw: log and keep the worker alive.
                    log::error("Network", std::string("io handler exception: ") + e.what());
                }
            }
        });
    }
}

IoRuntime::~IoRuntime() { stop(); }

void IoRuntime::stop() {
    if (stopped_.exchange(true)) return;
    guard_.reset();
    ctx_.stop();
    for (auto& t : threads_) {
        if (t.joinable()) {
            if (t.get_id() == std::this_thread::get_id()) {
                t.detach();
            } else {
                t.join();
            }
        }
    }
}

// ------------------------------------------------------------- helpers

Error error_from_asio(const std::error_code& ec, std::string_view what) {
    ErrorCode code = ErrorCode::Io;
    if (ec == asio::error::eof || ec == asio::error::connection_reset || ec == asio::error::connection_aborted ||
        ec == asio::error::broken_pipe)
        code = ErrorCode::ConnectionLost;
    else if (ec == asio::error::connection_refused)
        code = ErrorCode::ConnectionRefused;
    else if (ec == asio::error::host_unreachable || ec == asio::error::network_unreachable ||
             ec == asio::error::host_not_found)
        code = ErrorCode::HostUnreachable;
    else if (ec == asio::error::timed_out)
        code = ErrorCode::Timeout;
    else if (ec == asio::error::operation_aborted)
        code = ErrorCode::Cancelled;
    else if (ec.category() == asio::error::get_ssl_category())
        code = ErrorCode::TlsFailure;
    return Error{code, std::string(what) + ": " + ec.message()};
}

void apply_socket_options(tcp::socket& s, const NetworkTuning& t, const KeepaliveConfig& k) {
    std::error_code ec;
    s.set_option(tcp::no_delay(t.tcp_nodelay), ec);
    s.set_option(asio::socket_base::keep_alive(k.tcp_keepalive), ec);
    if (t.socket_buffer_size) {
        s.set_option(asio::socket_base::send_buffer_size(static_cast<int>(t.socket_buffer_size)), ec);
        s.set_option(asio::socket_base::receive_buffer_size(static_cast<int>(t.socket_buffer_size)), ec);
    }
}

static void restrict_to_tls13(asio::ssl::context& ctx) {
    SSL_CTX* c = ctx.native_handle();
    SSL_CTX_set_min_proto_version(c, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(c, TLS1_3_VERSION);
    SSL_CTX_set_options(c, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
}

Result<std::shared_ptr<asio::ssl::context>> make_server_tls_context(const std::string& cert, const std::string& key) {
    auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
    restrict_to_tls13(*ctx);
    std::error_code ec;
    ctx->use_certificate_chain_file(cert, ec);
    if (ec) return Error{ErrorCode::TlsFailure, "certificate " + cert + ": " + ec.message()};
    ctx->use_private_key_file(key, asio::ssl::context::pem, ec);
    if (ec) return Error{ErrorCode::TlsFailure, "private key " + key + ": " + ec.message()};
    return ctx;
}

std::shared_ptr<asio::ssl::context> make_client_tls_context() {
    auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
    restrict_to_tls13(*ctx);
    // Server identity is verified by certificate fingerprint pinning after the
    // handshake (self-signed server certificates; no public CA involved).
    ctx->set_verify_mode(asio::ssl::verify_none);
    return ctx;
}

// ------------------------------------------------------------- transports

namespace {

class TcpTransport final : public Transport {
public:
    explicit TcpTransport(tcp::socket s, Strand strand) : strand_(std::move(strand)), sock_(std::move(s)) {
        std::error_code ec;
        auto ep = sock_.remote_endpoint(ec);
        if (!ec) remote_ = ep.address().to_string() + ":" + std::to_string(ep.port());
    }
    void async_read_some(MutableByteSpan buf, IoHandler h) override {
        sock_.async_read_some(asio::buffer(buf.data(), buf.size()), asio::bind_executor(strand_, std::move(h)));
    }
    void async_write(const std::vector<asio::const_buffer>& bufs, IoHandler h) override {
        asio::async_write(sock_, bufs, asio::bind_executor(strand_, std::move(h)));
    }
    void close() noexcept override {
        std::error_code ec;
        sock_.shutdown(tcp::socket::shutdown_both, ec);
        sock_.close(ec);
    }
    bool is_secure() const noexcept override { return false; }
    std::string remote_address() const override { return remote_; }
    Strand& strand() noexcept override { return strand_; }

private:
    Strand strand_;
    tcp::socket sock_;
    std::string remote_;
};

std::string fingerprint_of(SSL* ssl) {
    X509* cert = SSL_get1_peer_certificate(ssl);
    if (!cert) return {};
    int len = i2d_X509(cert, nullptr);
    std::string fp;
    if (len > 0) {
        Bytes der(static_cast<std::size_t>(len));
        unsigned char* p = der.data();
        i2d_X509(cert, &p);
        fp = crypto::fingerprint_from_der(der);
    }
    X509_free(cert);
    return fp;
}

class TlsTransport final : public Transport {
public:
    TlsTransport(asio::ssl::stream<tcp::socket> s, Strand strand, std::shared_ptr<asio::ssl::context> ctx)
        : strand_(std::move(strand)), ctx_(std::move(ctx)), stream_(std::move(s)) {
        std::error_code ec;
        auto ep = stream_.lowest_layer().remote_endpoint(ec);
        if (!ec) remote_ = ep.address().to_string() + ":" + std::to_string(ep.port());
        fingerprint_ = fingerprint_of(stream_.native_handle());
    }
    void async_read_some(MutableByteSpan buf, IoHandler h) override {
        stream_.async_read_some(asio::buffer(buf.data(), buf.size()), asio::bind_executor(strand_, std::move(h)));
    }
    void async_write(const std::vector<asio::const_buffer>& bufs, IoHandler h) override {
        asio::async_write(stream_, bufs, asio::bind_executor(strand_, std::move(h)));
    }
    void close() noexcept override {
        // No TLS close_notify round trip: the PFUP GOODBYE frame already signals intent
        // and we must not block on an unresponsive peer.
        std::error_code ec;
        stream_.lowest_layer().shutdown(tcp::socket::shutdown_both, ec);
        stream_.lowest_layer().close(ec);
    }
    bool is_secure() const noexcept override { return true; }
    std::string remote_address() const override { return remote_; }
    std::string peer_fingerprint() const override { return fingerprint_; }
    Strand& strand() noexcept override { return strand_; }

private:
    Strand strand_;
    std::shared_ptr<asio::ssl::context> ctx_;  // must outlive the stream
    asio::ssl::stream<tcp::socket> stream_;
    std::string remote_;
    std::string fingerprint_;
};

}  // namespace

// ------------------------------------------------------------- listener

Listener::Listener(IoRuntime& rt, ListenerOptions opts) : rt_(rt), opts_(std::move(opts)), acceptor_(rt.context()) {}

Status Listener::start(AcceptHandler on_accept) {
    on_accept_ = std::move(on_accept);
    std::error_code ec;
    auto addr = asio::ip::make_address(opts_.address, ec);
    if (ec) return {ErrorCode::InvalidArgument, "listen address: " + ec.message()};
    tcp::endpoint ep(addr, opts_.port);
    acceptor_.open(ep.protocol(), ec);
    if (ec) return error_from_asio(ec, "open");
    acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
    acceptor_.bind(ep, ec);
    if (ec) return error_from_asio(ec, "bind " + opts_.address + ":" + std::to_string(opts_.port));
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) return error_from_asio(ec, "listen");
    bound_port_ = acceptor_.local_endpoint(ec).port();
    do_accept();
    return {};
}

void Listener::stop() {
    if (stopped_.exchange(true)) return;
    asio::post(acceptor_.get_executor(), [self = shared_from_this()] {
        std::error_code ec;
        self->acceptor_.close(ec);
    });
}

void Listener::do_accept() {
    auto strand = std::make_shared<Strand>(rt_.make_strand());
    acceptor_.async_accept(*strand, [self = shared_from_this(), strand](std::error_code ec, tcp::socket sock) {
        if (self->stopped_) return;
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                log::warn("Network", "accept failed: " + ec.message());
                // Back off briefly on resource errors (e.g. EMFILE) instead of spinning.
                auto t = std::make_shared<asio::steady_timer>(self->rt_.context(), std::chrono::milliseconds(100));
                t->async_wait([self, t](std::error_code) { if (!self->stopped_) self->do_accept(); });
            }
            return;
        }
        apply_socket_options(sock, self->opts_.tuning, self->opts_.keepalive);
        if (!self->opts_.tls) {
            self->on_accept_(std::make_unique<TcpTransport>(std::move(sock), *strand));
        } else {
            auto stream = std::make_shared<asio::ssl::stream<tcp::socket>>(std::move(sock), *self->opts_.tls);
            auto timer = std::make_shared<asio::steady_timer>(
                *strand, std::chrono::milliseconds(self->opts_.handshake_timeout_ms));
            timer->async_wait([stream](std::error_code tec) {
                if (!tec) {
                    std::error_code ignore;
                    stream->lowest_layer().close(ignore);
                }
            });
            stream->async_handshake(
                asio::ssl::stream_base::server,
                asio::bind_executor(*strand, [self, stream, strand, timer](std::error_code hec) {
                    timer->cancel();
                    if (hec) {
                        log::warn("Security", "TLS handshake failed: " + hec.message());
                        std::error_code ignore;
                        stream->lowest_layer().close(ignore);
                        return;
                    }
                    if (self->stopped_) return;
                    self->on_accept_(std::make_unique<TlsTransport>(std::move(*stream), *strand, self->opts_.tls));
                }));
        }
        self->do_accept();
    });
}

// ------------------------------------------------------------- connector

namespace {
struct ConnectOp : std::enable_shared_from_this<ConnectOp> {
    ConnectOp(IoRuntime& r, ConnectOptions o, ConnectHandler h)
        : rt(r), opts(std::move(o)), handler(std::move(h)), strand(r.make_strand()), resolver(strand),
          socket(strand), timer(strand) {}

    IoRuntime& rt;
    ConnectOptions opts;
    ConnectHandler handler;
    Strand strand;
    tcp::resolver resolver;
    tcp::socket socket;
    asio::steady_timer timer;
    std::shared_ptr<asio::ssl::context> tls_ctx;
    std::unique_ptr<asio::ssl::stream<tcp::socket>> tls_stream;
    bool done = false;

    void finish(Result<TransportPtr> r) {
        if (done) return;
        done = true;
        timer.cancel();
        handler(std::move(r));
    }

    void start() {
        timer.expires_after(std::chrono::milliseconds(opts.timeout_ms));
        timer.async_wait([self = shared_from_this()](std::error_code ec) {
            if (ec || self->done) return;
            std::error_code ignore;
            self->resolver.cancel();
            self->socket.close(ignore);
            if (self->tls_stream) self->tls_stream->lowest_layer().close(ignore);
            self->finish(Error{ErrorCode::Timeout, "connect timeout to " + self->opts.host});
        });
        resolver.async_resolve(opts.host, std::to_string(opts.port),
                               [self = shared_from_this()](std::error_code ec, tcp::resolver::results_type res) {
                                   if (self->done) return;
                                   if (ec) return self->finish(error_from_asio(ec, "resolve " + self->opts.host));
                                   asio::async_connect(self->socket, res,
                                                       [self](std::error_code cec, const tcp::endpoint&) {
                                                           self->on_connected(cec);
                                                       });
                               });
    }

    void on_connected(std::error_code ec) {
        if (done) return;
        if (ec) return finish(error_from_asio(ec, "connect " + opts.host + ":" + std::to_string(opts.port)));
        apply_socket_options(socket, opts.tuning, opts.keepalive);
        if (!opts.tls) return finish(TransportPtr(std::make_unique<TcpTransport>(std::move(socket), strand)));
        tls_ctx = make_client_tls_context();
        tls_stream = std::make_unique<asio::ssl::stream<tcp::socket>>(std::move(socket), *tls_ctx);
        SSL_set_tlsext_host_name(tls_stream->native_handle(), opts.host.c_str());
        tls_stream->async_handshake(asio::ssl::stream_base::client,
                                    [self = shared_from_this()](std::error_code hec) { self->on_handshake(hec); });
    }

    void on_handshake(std::error_code ec) {
        if (done) return;
        if (ec) return finish(error_from_asio(ec, "TLS handshake"));
        auto t = std::make_unique<TlsTransport>(std::move(*tls_stream), strand, tls_ctx);
        if (!opts.expected_fingerprint.empty() &&
            crypto::normalize_fingerprint(t->peer_fingerprint()) !=
                crypto::normalize_fingerprint(opts.expected_fingerprint)) {
            t->close();
            return finish(Error{ErrorCode::TlsFailure,
                                "server certificate fingerprint mismatch (possible MITM or server key changed)"});
        }
        finish(TransportPtr(std::move(t)));
    }
};
}  // namespace

void async_connect(IoRuntime& rt, ConnectOptions opts, ConnectHandler h) {
    auto op = std::make_shared<ConnectOp>(rt, std::move(opts), std::move(h));
    asio::post(op->strand, [op] { op->start(); });
}

}  // namespace pf::net
