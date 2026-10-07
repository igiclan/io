// Real-socket tests of the transport + FrameConnection (plaintext and TLS 1.3).
#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <future>
#include <map>
#include <nlohmann/json.hpp>
#include <mutex>

#include "../Common/TestEnv.hpp"
#include "Common/Platform.hpp"
#include "Network/FrameConnection.hpp"

using namespace pf;
using namespace pf::net;
using namespace std::chrono_literals;

namespace {

struct Pair {
    std::shared_ptr<FrameConnection> server, client;
};

struct Fixture {
    IoRuntime rt{2};
    test::TempDir dir;
    std::shared_ptr<asio::ssl::context> server_tls;
    std::string fingerprint;
    std::shared_ptr<Listener> listener;

    explicit Fixture(bool tls) {
        if (tls) {
            crypto::CertificateFiles f{dir / "s.crt", dir / "s.key"};
            REQUIRE(crypto::ensure_self_signed_certificate(f, "test").ok());
            auto ctx = make_server_tls_context(f.cert_pem.string(), f.key_pem.string());
            REQUIRE(ctx.ok());
            server_tls = *ctx;
            fingerprint = *crypto::certificate_fingerprint_file(f.cert_pem);
        }
    }
    ~Fixture() {
        if (listener) listener->stop();
        rt.stop();
    }

    Result<Pair> connect(ConnectionOptions opts, const std::string& expected_fp = {}) {
        ListenerOptions lo;
        lo.address = "127.0.0.1";
        lo.port = 0;
        lo.tls = server_tls;
        std::promise<TransportPtr> accepted;
        auto accepted_f = accepted.get_future();
        std::once_flag once;
        listener = std::make_shared<Listener>(rt, lo);
        auto st = listener->start([&](TransportPtr t) {
            std::call_once(once, [&] { accepted.set_value(std::move(t)); });
        });
        REQUIRE(st.ok());
        ConnectOptions co;
        co.host = "127.0.0.1";
        co.port = listener->port();
        co.tls = static_cast<bool>(server_tls);
        co.expected_fingerprint = expected_fp;
        std::promise<Result<TransportPtr>> connected;
        async_connect(rt, co, [&](Result<TransportPtr> r) { connected.set_value(std::move(r)); });
        auto cr = connected.get_future().get();
        if (!cr.ok()) return cr.error();
        REQUIRE(accepted_f.wait_for(5s) == std::future_status::ready);
        Pair p;
        p.server = FrameConnection::create(accepted_f.get(), opts);
        p.client = FrameConnection::create(std::move(cr.value()), opts);
        return p;
    }
};

void run_echo(bool tls) {
    Fixture fx(tls);
    ConnectionOptions opts;
    auto pair = fx.connect(opts, fx.fingerprint);
    REQUIRE(pair.ok());
    auto& [server, client] = *pair;
    if (tls) {
        CHECK(client->is_secure());
        CHECK(crypto::normalize_fingerprint(client->peer_fingerprint()) == crypto::normalize_fingerprint(fx.fingerprint));
    }
    // server echoes USB_SUBMIT OUT payloads back as USB_COMPLETE
    std::weak_ptr<FrameConnection> ws = server;
    server->start({[ws](proto::InFrame&& f) {
                       auto s = ws.lock();
                       if (!s || f.header.type != proto::MsgType::UsbSubmit) return;
                       proto::UsbSubmitMsg m;
                       REQUIRE(proto::decode_submit(f.bytes(), m).ok());
                       proto::UsbCompleteMsg c;
                       c.seq = m.seq;
                       c.actual_length = m.length;
                       c.in_data = m.out_data;
                       s->send(proto::encode_complete(c));
                   },
                   nullptr, nullptr});
    std::mutex mu;
    std::map<std::uint64_t, std::size_t> got;
    std::atomic<bool> closed{false};
    client->start({[&](proto::InFrame&& f) {
                       proto::UsbCompleteMsg c;
                       REQUIRE(proto::decode_complete(f.bytes(), c).ok());
                       std::lock_guard lk(mu);
                       got[c.seq] = c.in_data.size();
                   },
                   [&](const Error&) { closed = true; }, nullptr});
    const std::vector<std::size_t> sizes{0, 1, 512, 16384, 65536, 1 << 20, 4 << 20, 3};
    Bytes data(4 << 20, 0x5A);
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        proto::UsbSubmitMsg m;
        m.seq = i + 1;
        m.endpoint = 0x01;
        m.out_data = ByteSpan(data.data(), sizes[i]);
        client->send(proto::encode_submit(m));
    }
    REQUIRE(test::wait_for([&] { std::lock_guard lk(mu); return got.size() == sizes.size(); }));
    for (std::size_t i = 0; i < sizes.size(); ++i) CHECK(got[i + 1] == sizes[i]);
    CHECK(client->metrics()->tx.total() > (5u << 20));
    client->close(Error{ErrorCode::Ok, "done"});
    CHECK(test::wait_for([&] { return closed.load(); }));
}

}  // namespace

TEST_CASE("plaintext frame echo over TCP") { run_echo(false); }
TEST_CASE("TLS 1.3 frame echo with pinned fingerprint") { run_echo(true); }

TEST_CASE("TLS connection refused on fingerprint mismatch") {
    Fixture fx(true);
    auto pair = fx.connect({}, "00:11:22");
    REQUIRE_FALSE(pair.ok());
    CHECK(pair.error().code == ErrorCode::TlsFailure);
}

TEST_CASE("plaintext client cannot talk to TLS server") {
    Fixture fx(true);
    ListenerOptions lo;
    lo.address = "127.0.0.1";
    lo.port = 0;
    lo.tls = fx.server_tls;
    lo.handshake_timeout_ms = 500;
    std::atomic<int> accepted{0};
    auto l = std::make_shared<Listener>(fx.rt, lo);
    REQUIRE(l->start([&](TransportPtr) { ++accepted; }).ok());
    ConnectOptions co;
    co.host = "127.0.0.1";
    co.port = l->port();
    co.tls = false;
    std::promise<Result<TransportPtr>> p;
    async_connect(fx.rt, co, [&](Result<TransportPtr> r) { p.set_value(std::move(r)); });
    auto r = p.get_future().get();
    REQUIRE(r.ok());
    auto conn = FrameConnection::create(std::move(r.value()), {});
    std::atomic<bool> closed{false};
    conn->start({nullptr, [&](const Error&) { closed = true; }, nullptr});
    conn->send(proto::encode_json(proto::MsgType::Hello, nlohmann::json{{"x", 1}}));
    CHECK(test::wait_for([&] { return closed.load(); }, 5s));
    CHECK(accepted == 0);
    l->stop();
}

TEST_CASE("keepalive measures RTT and detects a dead peer") {
    Fixture fx(false);
    ConnectionOptions opts;
    opts.keepalive.interval_ms = 100;
    opts.keepalive.miss_limit = 2;
    auto pair = fx.connect(opts);
    REQUIRE(pair.ok());
    auto& [server, client] = *pair;
    std::atomic<bool> client_closed{false};
    Error reason;
    server->start({nullptr, nullptr, nullptr});
    client->start({nullptr, [&](const Error& e) { reason = e; client_closed = true; }, nullptr});
    REQUIRE(test::wait_for([&] { return client->metrics()->pongs_received.get() >= 2; }));
    CHECK(client->metrics()->rtt.snapshot().count >= 2);
    // Freeze the server: stop reading so it never answers pings. TCP stays up.
    server->pause_reading();
    REQUIRE(test::wait_for([&] { return client_closed.load(); }, 5s));
    CHECK(reason.code == ErrorCode::ConnectionLost);
}

TEST_CASE("backpressure signals above high watermark and clears below low") {
    Fixture fx(false);
    ConnectionOptions opts;
    opts.tuning.send_high_watermark = 1 << 20;
    opts.tuning.send_low_watermark = 256 << 10;
    opts.send_pings = false;
    auto pair = fx.connect(opts);
    REQUIRE(pair.ok());
    auto& [server, client] = *pair;
    std::atomic<int> on{0}, off{0};
    std::atomic<std::size_t> received{0};
    server->start({[&](proto::InFrame&&) { ++received; }, nullptr, nullptr});
    server->pause_reading();
    client->start({nullptr, nullptr, [&](bool c) { (c ? on : off)++; }});
    Bytes chunk(256 << 10, 1);
    for (int i = 0; i < 64; ++i) {
        proto::UsbSubmitMsg m;
        m.seq = static_cast<std::uint64_t>(i);
        m.endpoint = 0x02;
        m.out_data = chunk;
        client->send(proto::encode_submit(m));
    }
    REQUIRE(test::wait_for([&] { return on.load() == 1; }));
    CHECK(client->congested());
    server->resume_reading();
    REQUIRE(test::wait_for([&] { return received.load() == 64; }));
    CHECK(test::wait_for([&] { return off.load() == 1; }));
    CHECK_FALSE(client->congested());
}

TEST_CASE("malformed stream closes the connection with an ERROR frame") {
    Fixture fx(false);
    auto pair = fx.connect({});
    REQUIRE(pair.ok());
    auto& [server, client] = *pair;
    std::atomic<bool> server_closed{false};
    std::atomic<bool> got_error{false};
    server->start({nullptr, [&](const Error& e) { server_closed = e.code == ErrorCode::BadMagic; }, nullptr});
    client->start({[&](proto::InFrame&& f) { if (f.header.type == proto::MsgType::Error) got_error = true; },
                   nullptr, nullptr});
    proto::OutFrame junk;
    junk.buf = BufferPool::global().acquire(32);
    std::memset(junk.buf.data(), 'X', 32);
    client->send(std::move(junk));
    CHECK(test::wait_for([&] { return server_closed.load(); }));
    CHECK(test::wait_for([&] { return got_error.load(); }));
}
