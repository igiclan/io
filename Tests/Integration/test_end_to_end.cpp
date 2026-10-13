// End-to-end: TestVhci URB -> ClientSession -> TLS -> ServerEngine -> loopback device and back.
#include <doctest/doctest.h>

#include "Harness.hpp"

using namespace pf;
using namespace pf::test;
using pf::client::TestUrb;
using namespace std::chrono_literals;

TEST_CASE("attach, enumerate descriptors and move data over TLS") {
    Harness h;
    auto& c = h.add_client("PC-01");
    REQUIRE(h.wait_connected(c));
    auto id = h.device_on_port(1);
    REQUIRE_FALSE(id.empty());
    auto sid = c.attach_sync("bench", id);
    REQUIRE_MESSAGE(sid.ok(), (sid.ok() ? std::string() : sid.error().describe()));
    auto& v = h.vhci();
    const std::uint32_t dev = v.wait_for_device(0x18d1, 0x4ee7, 5s);
    REQUIRE(dev != 0);
    // GET_DESCRIPTOR(device) through the whole stack
    auto r = v.control_in(dev, 0x80, 0x06, 0x0100, 0, 18);
    REQUIRE(r.status == UsbStatus::Success);
    REQUIRE(r.data.size() == 18);
    CHECK(r.data[8] == 0xd1);
    CHECK(r.data[9] == 0x18);
    // vendor OUT then IN (control data both directions)
    Bytes payload{1, 2, 3, 4, 5, 6, 7};
    CHECK(v.control_out(dev, 0x40, 0x02, 0, 0, payload).status == UsbStatus::Success);
    auto back = v.control_in(dev, 0xC0, 0x03, 0, 0, 64);
    CHECK(back.data == payload);
    // bulk loopback, 1 MiB
    Bytes big(1 << 20);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 7);
    CHECK(v.bulk_out(dev, 0x01, big).status == UsbStatus::Success);
    Bytes got;
    while (got.size() < big.size()) {
        auto in = v.bulk_in(dev, 0x81, 256 * 1024);
        REQUIRE(in.status == UsbStatus::Success);
        got.insert(got.end(), in.data.begin(), in.data.end());
    }
    CHECK(got == big);
    // interrupt + unknown endpoint stall
    TestUrb irq;
    irq.endpoint = 0x82;
    irq.type = TransferType::Interrupt;
    irq.length = 8;
    auto ir = v.submit_urb(dev, irq).get();
    CHECK(ir.status == UsbStatus::Success);
    CHECK(ir.actual_length == 8);
    CHECK(v.bulk_in(dev, 0x8F, 64).status == UsbStatus::Stall);
    // SET_CONFIGURATION is routed through the host API, not EP0
    CHECK(v.control_out(dev, 0x00, 0x09, 1, 0, {}).status == UsbStatus::Success);
    CHECK(v.select_interface(dev, 0, 0).get() == UsbStatus::Success);
    // server state reflects ownership
    auto devs = h.server->devices();
    auto it = std::find_if(devs.begin(), devs.end(), [&](const DeviceInfo& d) { return d.id == id; });
    REQUIRE(it != devs.end());
    CHECK(it->state == DeviceShareState::InUse);
    CHECK(it->owner_name == "PC-01");
    // detach
    REQUIRE(c.detach(*sid).ok());
    CHECK(wait_for([&] { return v.plugged_count() == 0; }));
    CHECK(wait_for([&] {
        for (auto& d : h.server->devices())
            if (d.id == id) return d.state == DeviceShareState::Shared;
        return false;
    }));
}

TEST_CASE("isochronous IN transfer") {
    Harness h;
    auto& c = h.add_client("PC-01");
    REQUIRE(h.wait_connected(c));
    REQUIRE(c.attach_sync("bench", h.device_on_port(1)).ok());
    auto dev = h.vhci().wait_for_device(0x18d1, 0x4ee7, 5s);
    TestUrb u;
    u.endpoint = 0x85;
    u.type = TransferType::Isochronous;
    u.length = 3 * 1024;
    u.iso = {{0, 1024}, {1024, 1024}, {2048, 1024}};
    auto r = h.vhci().submit_urb(dev, u).get();
    REQUIRE(r.status == UsbStatus::Success);
    REQUIRE(r.iso.size() == 3);
    CHECK(r.iso[2].actual_length == 1024);
    REQUIRE(r.data.size() >= 3072);
    CHECK(r.data[0] == 0);
    CHECK(r.data[1024] == 1);
    CHECK(r.data[2048] == 2);
}

TEST_CASE("exclusive ownership: second client is refused, can attach after release") {
    Harness h;
    auto& a = h.add_client("PC-01");
    auto& b = h.add_client("PC-02");
    REQUIRE(h.wait_connected(a));
    REQUIRE(h.wait_connected(b));
    const auto id = h.device_on_port(1);
    auto s1 = a.attach_sync("bench", id);
    REQUIRE(s1.ok());
    auto s2 = b.attach_sync("bench", id);
    REQUIRE_FALSE(s2.ok());
    CHECK(s2.error().code == ErrorCode::DeviceBusy);
    CHECK(s2.error().message.find("PC-01") != std::string::npos);
    // multi-device: PC-02 takes the other device
    CHECK(b.attach_sync("bench", h.device_on_port(2)).ok());
    REQUIRE(a.detach(*s1).ok());
    CHECK(wait_for([&] { return b.attach_sync("bench", id).ok(); }, 5s));
}

TEST_CASE("shared read-only observer cannot submit") {
    Harness h;
    auto& a = h.add_client("PC-01");
    REQUIRE(h.wait_connected(a));
    auto s = a.attach_sync("bench", h.device_on_port(1), AccessMode::SharedReadOnly);
    REQUIRE(s.ok());
    CHECK(h.vhci().plugged_count() == 0);  // observers do not get a virtual device
    CHECK(a.session(*s)->state() == SessionState::Active);
}

TEST_CASE("authentication is enforced") {
    Harness h;
    auto& bad = h.add_client("intruder", "pf_00000000_" + std::string(64, '0'));
    CHECK(wait_for([&] {
        auto s = bad.servers();
        return !s.empty() && s[0].state == client::LinkState::Failed;
    }));
    CHECK(bad.servers()[0].devices == 0);
}

TEST_CASE("cancelled URB is cancelled on the server") {
    Harness h;
    auto& c = h.add_client("PC-01");
    REQUIRE(h.wait_connected(c));
    REQUIRE(c.attach_sync("bench", h.device_on_port(1)).ok());
    auto dev = h.vhci().wait_for_device(0x18d1, 0x4ee7, 5s);
    TestUrb u;
    u.endpoint = 0x81;  // pends: FIFO empty
    u.length = 512;
    std::uint64_t rid = 0;
    auto fut = h.vhci().submit_urb(dev, u, &rid);
    CHECK(fut.wait_for(200ms) == std::future_status::timeout);
    h.vhci().cancel_urb(dev, rid);
    CHECK(fut.get().status == UsbStatus::Cancelled);
    // the server-side transfer was cancelled: new data goes to the next reader
    CHECK(h.vhci().bulk_out(dev, 0x01, Bytes{9, 9}).status == UsbStatus::Success);
    auto r = h.vhci().bulk_in(dev, 0x81, 512);
    CHECK(r.status == UsbStatus::Success);
    CHECK(r.data == Bytes{9, 9});
}
