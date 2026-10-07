#include <doctest/doctest.h>

#include "Models/DeviceIdentity.hpp"

using namespace pf;

static DeviceIdentity phone(std::uint16_t pid, std::string serial, std::string port) {
    DeviceIdentity d;
    d.vid = 0x18d1;
    d.pid = pid;
    d.serial = std::move(serial);
    d.port_path = std::move(port);
    d.parent_id = "hub-1";
    return d;
}

TEST_CASE("placeholder serials") {
    CHECK(is_placeholder_serial(""));
    CHECK(is_placeholder_serial("0123456789ABCDEF"));
    CHECK(is_placeholder_serial("0123456789abcdef"));
    CHECK(is_placeholder_serial("00000000"));
    CHECK_FALSE(is_placeholder_serial("R58M123ABC"));
}

TEST_CASE("same phone switching ADB -> fastboot on same port matches") {
    IdentityMatcher m;
    auto adb = phone(0x4ee7, "28131FDH2000AB", "1-4.2");
    auto fb = phone(0x4ee0, "28131FDH2000AB", "1-4.2");
    auto r = m.match(adb, 1000, fb, 3000);
    CHECK(r.matched);
    CHECK(r.score >= 100);
}

TEST_CASE("EDL mode without serial on the same port matches") {
    IdentityMatcher m;
    auto normal = phone(0x4ee7, "R58M123ABC", "1-3");
    DeviceIdentity edl;
    edl.vid = 0x05c6;
    edl.pid = 0x9008;
    edl.port_path = "1-3";
    edl.parent_id = "hub-1";
    CHECK(m.match(normal, 0, edl, 2000).matched);
}

TEST_CASE("two identical phones on different ports are not confused") {
    IdentityMatcher m;
    auto a = phone(0x9008, "", "1-1");
    auto b = phone(0x9008, "", "1-2");
    CHECK_FALSE(m.match(a, 0, b, 1000).matched);
}

TEST_CASE("different serial never matches even on same port") {
    IdentityMatcher m;
    auto a = phone(0x4ee7, "AAA111", "1-1");
    auto b = phone(0x4ee7, "BBB222", "1-1");
    CHECK_FALSE(m.match(a, 0, b, 500).matched);
}

TEST_CASE("serial match survives a port change") {
    IdentityMatcher m;
    auto a = phone(0x4ee7, "R58M123ABC", "1-1");
    auto b = phone(0x4ee0, "R58M123ABC", "2-5");
    CHECK(m.match(a, 0, b, 5000).matched);
}

TEST_CASE("expired removal is never matched") {
    IdentityMatcher m;
    auto a = phone(0x4ee7, "R58M123ABC", "1-1");
    CHECK_FALSE(m.match(a, 0, a, 120000).matched);
}

TEST_CASE("device id is stable and port based") {
    auto a = phone(0x4ee7, "X", "1-1");
    CHECK(make_device_id(a) == make_device_id(a));
    auto b = a;
    b.port_path = "1-2";
    CHECK(make_device_id(a) != make_device_id(b));
    CHECK(make_device_id(a).size() == 13);
}
