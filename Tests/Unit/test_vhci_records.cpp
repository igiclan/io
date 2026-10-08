#include <doctest/doctest.h>

#include <random>

#include "../../Shared/Driver/pfvhci_ioctl.h"
#include "Models/DescriptorBuilder.hpp"
#include "VirtualUSB/VhciRecords.hpp"

using namespace pf;
using namespace pf::client;

TEST_CASE("PLUGIN buffer round trip and validation") {
    DeviceSpec spec;
    spec.serial = "ABC";
    spec.interfaces.push_back({0, 0, 0xff, 0x42, 0x01, "ADB", {{0x81, TransferType::Bulk, 512}}});
    auto set = build_descriptor_set(spec);
    Bytes buf = encode_plugin(set, UsbSpeed::High, 77);
    CHECK(buf.size() % 8 == 0);
    UsbSpeed sp{};
    std::uint64_t cookie = 0;
    auto back = decode_plugin(buf, sp, cookie);
    REQUIRE(back.ok());
    CHECK(cookie == 77);
    CHECK(sp == UsbSpeed::High);
    CHECK(back->device == set.device);
    CHECK(back->strings.size() == set.strings.size());
    for (std::size_t n = 0; n < buf.size(); n += 3) CHECK_FALSE(decode_plugin(ByteSpan(buf.data(), n), sp, cookie).ok());
    Bytes bad = buf;
    bad[4] = 99;  // ABI version
    CHECK(decode_plugin(bad, sp, cookie).error().code == ErrorCode::DriverVersionMismatch);
}

TEST_CASE("FETCH record stream round trip with batching") {
    Bytes stream;
    VhciRequest a;
    a.kind = VhciRecordKind::Submit;
    a.device_handle = 3;
    a.request_id = 10;
    a.endpoint = 0x01;
    Bytes data(13, 0xAB);
    a.out_data = data;
    append_request(stream, a);
    VhciRequest b;
    b.kind = VhciRecordKind::SelectInterface;
    b.device_handle = 3;
    b.request_id = 11;
    b.interface_number = 1;
    b.alt_setting = 2;
    append_request(stream, b);
    CHECK(stream.size() % 8 == 0);
    auto recs = parse_fetch_buffer(stream);
    REQUIRE(recs.ok());
    REQUIRE(recs->size() == 2);
    CHECK((*recs)[0].out_data.size() == 13);
    CHECK((*recs)[1].alt_setting == 2);
}

TEST_CASE("FETCH parser rejects corrupt kernel buffers without crashing") {
    VhciRequest a;
    a.kind = VhciRecordKind::Submit;
    a.endpoint = 0x02;
    Bytes data(100, 1);
    a.out_data = data;
    Bytes stream;
    append_request(stream, a);
    for (std::size_t n = 1; n < stream.size(); ++n) CHECK_FALSE(parse_fetch_buffer(ByteSpan(stream.data(), n)).ok());
    std::mt19937 rng(5);
    for (int i = 0; i < 3000; ++i) {
        Bytes junk(rng() % 256);
        for (auto& x : junk) x = static_cast<std::uint8_t>(rng());
        (void)parse_fetch_buffer(junk);
        (void)parse_complete_buffer(junk);
    }
}

TEST_CASE("COMPLETION record layout matches the ABI header") {
    Bytes in_data{1, 2, 3};
    VhciCompletion c;
    c.device_handle = 4;
    c.request_id = 99;
    c.status = UsbStatus::Stall;
    c.actual_length = 3;
    c.in_data = in_data;
    Bytes out;
    append_completion(out, c);
    REQUIRE(out.size() == PFVHCI_ALIGN8(sizeof(PFVHCI_COMPLETION) + 3));
    CHECK(load_le32(out.data()) == out.size());
    CHECK(load_le16(out.data() + 4) == PFVHCI_REC_COMPLETION);
    CHECK(load_le64(out.data() + 24) == 99);
    CHECK(static_cast<std::int32_t>(load_le32(out.data() + 32)) == PFVHCI_STATUS_STALL);
    CHECK(out[sizeof(PFVHCI_COMPLETION)] == 1);
    auto parsed = parse_complete_buffer(out);
    REQUIRE(parsed.ok());
    CHECK((*parsed)[0].actual_length == 3);
}
