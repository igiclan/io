#include <doctest/doctest.h>

#include <cstring>
#include <nlohmann/json.hpp>
#include <random>

#include "Protocol/Protocol.hpp"

using namespace pf;
using namespace pf::proto;

namespace {
// Feeds `stream` into a decoder in chunks of `chunk` bytes; collects frames.
std::vector<InFrame> decode_all(const Bytes& stream, std::size_t chunk, Status* status = nullptr) {
    FrameDecoder dec(kDefaultMaxPayload, 8192);
    std::vector<InFrame> out;
    std::size_t pos = 0;
    while (pos < stream.size()) {
        auto span = dec.prepare();
        std::size_t n = std::min({span.size(), chunk, stream.size() - pos});
        std::memcpy(span.data(), stream.data() + pos, n);
        pos += n;
        auto st = dec.commit(n, [&](InFrame&& f) { out.push_back(std::move(f)); });
        if (!st) {
            if (status) *status = st;
            return out;
        }
    }
    return out;
}
void append(Bytes& stream, const OutFrame& f) { stream.insert(stream.end(), f.buf.data(), f.buf.data() + f.buf.size()); }
}  // namespace

TEST_CASE("header encode/decode and validation") {
    std::uint8_t h[kHeaderSize];
    FrameHeader in;
    in.type = MsgType::UsbSubmit;
    in.flags = frame_flags::kResponse;
    in.payload_size = 1234;
    in.correlation = 77;
    encode_header(h, in);
    CHECK(h[0] == 'P');
    CHECK(h[1] == 'F');
    auto out = decode_header(h, kDefaultMaxPayload);
    REQUIRE(out.ok());
    CHECK(out->type == MsgType::UsbSubmit);
    CHECK(out->payload_size == 1234);
    CHECK(out->correlation == 77);

    CHECK(decode_header(h, 100).error().code == ErrorCode::FrameTooLarge);
    h[2] = 9;
    CHECK(decode_header(h, kDefaultMaxPayload).error().code == ErrorCode::UnsupportedVersion);
    h[2] = 1;
    h[3] = 0x99;
    CHECK(decode_header(h, kDefaultMaxPayload).error().code == ErrorCode::ProtocolViolation);
    h[0] = 'X';
    CHECK(decode_header(h, kDefaultMaxPayload).error().code == ErrorCode::BadMagic);
}

TEST_CASE("USB_SUBMIT OUT bulk round trip") {
    Bytes data(3000);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i);
    UsbSubmitMsg m;
    m.seq = 0x1122334455667788ULL;
    m.device = 7;
    m.endpoint = 0x01;
    m.type = TransferType::Bulk;
    m.flags = xfer_flags::kZeroPacket;
    m.timeout_ms = 5000;
    m.out_data = data;
    auto f = encode_submit(m);
    CHECK(f.buf.size() == kHeaderSize + kSubmitFixedSize + data.size());
    auto hdr = decode_header(f.buf.data(), kDefaultMaxPayload);
    REQUIRE(hdr.ok());
    UsbSubmitMsg d;
    REQUIRE(decode_submit(f.buf.span().subspan(kHeaderSize), d).ok());
    CHECK(d.seq == m.seq);
    CHECK(d.device == 7);
    CHECK(d.endpoint == 1);
    CHECK(d.length == 3000);
    CHECK(d.flags == xfer_flags::kZeroPacket);
    CHECK(std::equal(d.out_data.begin(), d.out_data.end(), data.begin(), data.end()));
}

TEST_CASE("USB_SUBMIT control IN validates direction against setup packet") {
    UsbSubmitMsg m;
    m.type = TransferType::Control;
    m.endpoint = 0x80;
    m.flags = xfer_flags::kDirIn;
    m.length = 18;
    m.setup = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0};
    auto f = encode_submit(m);
    UsbSubmitMsg d;
    REQUIRE(decode_submit(f.buf.span().subspan(kHeaderSize), d).ok());
    CHECK(d.setup[1] == 0x06);
    CHECK(d.is_in());
    // flip bmRequestType direction -> mismatch must be rejected
    f.buf.data()[kHeaderSize + 24] = 0x00;
    CHECK(decode_submit(f.buf.span().subspan(kHeaderSize), d).error().code == ErrorCode::MalformedPayload);
}

TEST_CASE("USB_SUBMIT isochronous packets bounded by buffer") {
    UsbSubmitMsg m;
    m.type = TransferType::Isochronous;
    m.endpoint = 0x81;
    m.flags = xfer_flags::kDirIn;
    m.length = 3072;
    m.iso = {{0, 1024}, {1024, 1024}, {2048, 1024}};
    auto f = encode_submit(m);
    UsbSubmitMsg d;
    REQUIRE(decode_submit(f.buf.span().subspan(kHeaderSize), d).ok());
    CHECK(d.iso.size() == 3);
    CHECK(d.iso[2].offset == 2048);
    m.iso.push_back({3000, 1024});  // outside buffer
    auto bad = encode_submit(m);
    CHECK_FALSE(decode_submit(bad.buf.span().subspan(kHeaderSize), d).ok());
}

TEST_CASE("USB_COMPLETE round trip and length validation") {
    Bytes data{1, 2, 3, 4, 5};
    UsbCompleteMsg m;
    m.seq = 42;
    m.device = 3;
    m.status = UsbStatus::Success;
    m.actual_length = 5;
    m.in_data = data;
    auto f = encode_complete(m);
    UsbCompleteMsg d;
    REQUIRE(decode_complete(f.buf.span().subspan(kHeaderSize), d).ok());
    CHECK(d.seq == 42);
    CHECK(d.actual_length == 5);
    CHECK(d.in_data.size() == 5);
    m.actual_length = 6;
    auto bad = encode_complete(m);
    CHECK_FALSE(decode_complete(bad.buf.span().subspan(kHeaderSize), d).ok());
}

TEST_CASE("small control messages round trip") {
    UsbCancelMsg c{99, 4};
    UsbCancelMsg c2;
    REQUIRE(decode_cancel(encode_cancel(c).buf.span().subspan(kHeaderSize), c2).ok());
    CHECK(c2.seq == 99);
    UsbResetMsg r{5, 4, ResetKind::Reenumerate};
    UsbResetMsg r2;
    REQUIRE(decode_reset(encode_reset(r).buf.span().subspan(kHeaderSize), r2).ok());
    CHECK(r2.kind == ResetKind::Reenumerate);
    UsbConfigMsg cfg{6, 4, ConfigOp::SetInterface, 1, 2};
    UsbConfigMsg cfg2;
    REQUIRE(decode_config(encode_config(cfg).buf.span().subspan(kHeaderSize), cfg2).ok());
    CHECK(cfg2.op == ConfigOp::SetInterface);
    CHECK(cfg2.alt == 2);
    UsbOpResultMsg o{6, 4, UsbStatus::Stall};
    UsbOpResultMsg o2;
    REQUIRE(decode_op_result(encode_op_result(o).buf.span().subspan(kHeaderSize), o2).ok());
    CHECK(o2.status == UsbStatus::Stall);
    PingMsg p{1, 123456789};
    PingMsg p2;
    REQUIRE(decode_ping(encode_ping(MsgType::Ping, p).buf.span().subspan(kHeaderSize), p2).ok());
    CHECK(p2.sent_ns == 123456789);
}

TEST_CASE("JSON control frames") {
    nlohmann::json j{{"client_name", "PC-01"}, {"protocol", {1}}};
    auto f = encode_json(MsgType::Hello, j, 5);
    auto frames = decode_all(Bytes(f.buf.data(), f.buf.data() + f.buf.size()), 7);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].header.correlation == 5);
    auto back = decode_json(frames[0]);
    REQUIRE(back.ok());
    CHECK((*back)["client_name"] == "PC-01");
    auto e = encode_error(ErrorCode::DeviceBusy, "owned by PC-02", 9);
    auto ef = decode_all(Bytes(e.buf.data(), e.buf.data() + e.buf.size()), 1000);
    REQUIRE(ef.size() == 1);
    CHECK(ef[0].header.type == MsgType::Error);
    CHECK((*decode_json(ef[0]))["code"] == static_cast<int>(ErrorCode::DeviceBusy));
}

TEST_CASE("FrameDecoder handles arbitrary chunking and large direct payloads") {
    Bytes stream;
    std::vector<std::size_t> sizes{0, 1, 100, 5000, 70000, 3, 1 << 20, 17};
    std::mt19937 rng(1234);
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        Bytes data(sizes[i]);
        for (auto& b : data) b = static_cast<std::uint8_t>(rng());
        UsbCompleteMsg m;
        m.seq = i;
        m.actual_length = static_cast<std::uint32_t>(data.size());
        m.in_data = data;
        append(stream, encode_complete(m));
        append(stream, encode_ping(MsgType::Ping, PingMsg{i, i}));
    }
    for (std::size_t chunk : {1ul, 13ul, 4096ul, 65536ul, 10ul << 20}) {
        auto frames = decode_all(stream, chunk);
        REQUIRE(frames.size() == sizes.size() * 2);
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            UsbCompleteMsg d;
            REQUIRE(decode_complete(frames[2 * i].bytes(), d).ok());
            CHECK(d.seq == i);
            CHECK(d.in_data.size() == sizes[i]);
            CHECK(frames[2 * i + 1].header.type == MsgType::Ping);
        }
    }
}

TEST_CASE("FrameDecoder rejects garbage") {
    Bytes junk(64, 0x41);
    Status st;
    auto frames = decode_all(junk, 64, &st);
    CHECK(frames.empty());
    CHECK(st.error().code == ErrorCode::BadMagic);
}

TEST_CASE("decoders never read out of bounds on truncated input") {
    std::mt19937 rng(99);
    UsbSubmitMsg m;
    m.type = TransferType::Isochronous;
    m.endpoint = 0x02;
    m.iso = {{0, 4}, {4, 4}};
    Bytes data(8, 0xAA);
    m.out_data = data;
    auto f = encode_submit(m);
    auto payload = f.buf.span().subspan(kHeaderSize);
    for (std::size_t n = 0; n < payload.size(); ++n) {
        UsbSubmitMsg d;
        CHECK_FALSE(decode_submit(payload.first(n), d).ok());
    }
    // random fuzz: must not crash
    for (int iter = 0; iter < 2000; ++iter) {
        Bytes junk(rng() % 128);
        for (auto& b : junk) b = static_cast<std::uint8_t>(rng());
        UsbSubmitMsg s;
        UsbCompleteMsg c;
        (void)decode_submit(junk, s);
        (void)decode_complete(junk, c);
    }
}
