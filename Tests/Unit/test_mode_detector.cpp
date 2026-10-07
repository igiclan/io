#include <doctest/doctest.h>

#include "Models/DescriptorBuilder.hpp"
#include "Models/ModeDetector.hpp"

using namespace pf;

static DescriptorSet with_iface(std::uint16_t vid, std::uint16_t pid, std::uint8_t c, std::uint8_t s, std::uint8_t p,
                                std::string product = "Device") {
    DeviceSpec spec;
    spec.vid = vid;
    spec.pid = pid;
    spec.product = std::move(product);
    InterfaceSpec i;
    i.cls = c;
    i.subclass = s;
    i.protocol = p;
    i.endpoints = {{0x81, TransferType::Bulk, 512}, {0x01, TransferType::Bulk, 512}};
    spec.interfaces.push_back(i);
    return build_descriptor_set(spec);
}

TEST_CASE("mode detection by interface triplet works for any vendor") {
    ModeDetector d;
    CHECK(d.detect(with_iface(0x2717, 0xff48, 0xff, 0x42, 0x01)).id == "adb");
    CHECK(d.detect(with_iface(0x2a70, 0x1234, 0xff, 0x42, 0x03)).id == "fastboot");
    CHECK(d.detect(with_iface(0x1234, 0x5678, 0x08, 0x06, 0x50)).id == "mass-storage");
    CHECK(d.detect(with_iface(0x1234, 0x5678, 0xff, 0x00, 0x00)).id == "generic");
}

TEST_CASE("mode detection by VID/PID signatures") {
    ModeDetector d;
    auto edl = d.detect(with_iface(0x05c6, 0x9008, 0xff, 0xff, 0xff));
    CHECK(edl.id == "qualcomm-edl");
    CHECK(edl.repair_mode);
    CHECK(d.detect(with_iface(0x0e8d, 0x0003, 0x02, 0x02, 0x01)).id == "mtk-brom");
    CHECK(d.detect(with_iface(0x0e8d, 0x2000, 0x02, 0x02, 0x01)).id == "mtk-preloader");
    CHECK(d.detect(with_iface(0x04e8, 0x685d, 0x02, 0x02, 0x01)).id == "samsung-download");
}

TEST_CASE("product string signature") {
    ModeDetector d;
    CHECK(d.detect(with_iface(0x1004, 0x1111, 0x02, 0x02, 0x01, "MT65xx Preloader")).id == "mtk-preloader");
}

TEST_CASE("custom signatures can be loaded and take priority") {
    ModeDetector d;
    REQUIRE(d.load_json(R"({"signatures":[{"id":"oem-x","label":"OEM X Flash","priority":200,"vid":"abcd"}]})").ok());
    CHECK(d.detect(with_iface(0xabcd, 1, 0xff, 0x42, 0x01)).id == "oem-x");
    CHECK_FALSE(d.load_json(R"({"signatures":[{"id":"bad","vid":"zz"}]})").ok());
    CHECK_FALSE(d.load_json("not json").ok());
}
