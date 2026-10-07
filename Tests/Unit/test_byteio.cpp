#include <doctest/doctest.h>

#include "Common/ByteIo.hpp"

using namespace pf;

TEST_CASE("ByteWriter/ByteReader round trip little endian") {
    Bytes b;
    ByteWriter w(b);
    w.u8(0xAB);
    w.u16(0x1234);
    w.u32(0xDEADBEEF);
    w.u64(0x0102030405060708ULL);
    w.i32(-5);
    REQUIRE(b.size() == 19);
    CHECK(b[1] == 0x34);
    CHECK(b[2] == 0x12);
    ByteReader r(b);
    CHECK(r.u8() == 0xAB);
    CHECK(r.u16() == 0x1234);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0102030405060708ULL);
    CHECK(r.i32() == -5);
    CHECK(r.ok());
    CHECK(r.remaining() == 0);
    r.u8();
    CHECK_FALSE(r.ok());
}

TEST_CASE("SpanWriter refuses overflow") {
    std::uint8_t buf[5];
    SpanWriter w(buf);
    w.u32(1);
    CHECK(w.ok());
    w.u16(2);
    CHECK_FALSE(w.ok());
    CHECK(w.position() == 4);
}

TEST_CASE("hex and base64") {
    Bytes data{0x00, 0xFF, 0x10, 0x7A};
    CHECK(to_hex(data) == "00ff107a");
    Bytes back;
    REQUIRE(from_hex("00FF107a", back));
    CHECK(back == data);
    CHECK_FALSE(from_hex("0g", back));
    for (std::size_t n = 0; n < 10; ++n) {
        Bytes in;
        for (std::size_t i = 0; i < n; ++i) in.push_back(static_cast<std::uint8_t>(i * 37 + 1));
        auto enc = base64_encode(in);
        Bytes dec;
        REQUIRE(base64_decode(enc, dec));
        CHECK(dec == in);
    }
    CHECK(base64_encode(Bytes{'M', 'a', 'n'}) == "TWFu");
    Bytes dec;
    CHECK_FALSE(base64_decode("TW=u", dec));
    CHECK_FALSE(base64_decode("TWF", dec));
    CHECK_FALSE(base64_decode("T!Fu", dec));
}
