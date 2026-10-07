#include <doctest/doctest.h>

#include "Models/DescriptorBuilder.hpp"
#include "Models/Descriptors.hpp"

using namespace pf;

static DeviceSpec adb_spec() {
    DeviceSpec s;
    s.vid = 0x18d1;
    s.pid = 0x4ee7;
    s.product = "Pixel 7";
    s.serial = "28131FDH2000AB";
    InterfaceSpec adb;
    adb.number = 0;
    adb.cls = 0xff;
    adb.subclass = 0x42;
    adb.protocol = 0x01;
    adb.name = "ADB Interface";
    adb.endpoints = {{0x81, TransferType::Bulk, 512}, {0x01, TransferType::Bulk, 512}};
    s.interfaces.push_back(adb);
    return s;
}

TEST_CASE("descriptor builder output parses back") {
    auto set = build_descriptor_set(adb_spec());
    auto dd = parse_device_descriptor(set.device);
    REQUIRE(dd);
    CHECK(dd->idVendor == 0x18d1);
    CHECK(dd->idProduct == 0x4ee7);
    CHECK(find_string(set, dd->iProduct) == "Pixel 7");
    CHECK(find_string(set, dd->iSerialNumber) == "28131FDH2000AB");
    auto cfg = parse_configuration(set.configs[0]);
    REQUIRE(cfg);
    REQUIRE(cfg->interfaces.size() == 1);
    CHECK(cfg->interfaces[0].subclass == 0x42);
    REQUIRE(cfg->interfaces[0].endpoints.size() == 2);
    CHECK(cfg->interfaces[0].endpoints[0].is_in());
    CHECK(cfg->interfaces[0].endpoints[0].type() == TransferType::Bulk);
    std::string why;
    CHECK(validate_descriptor_set(set, &why));
}

TEST_CASE("malformed configuration descriptors are rejected") {
    auto set = build_descriptor_set(adb_spec());
    Bytes cfg = set.configs[0];
    cfg[9] = 0;  // first sub-descriptor bLength = 0
    CHECK_FALSE(parse_configuration(cfg));
    Bytes truncated(set.configs[0].begin(), set.configs[0].begin() + 12);
    CHECK_FALSE(parse_configuration(truncated));
    set.device[7] = 13;
    CHECK_FALSE(validate_descriptor_set(set));
}

TEST_CASE("string descriptor UTF-16 round trip including non-ASCII") {
    auto raw = make_string_descriptor("پارس فلش");
    CHECK(raw[0] == raw.size());
    CHECK(decode_string_descriptor(raw) == "پارس فلش");
    CHECK(format_bcd(0x0210) == "2.10");
}
