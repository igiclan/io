// Smart session persistence: network loss + resume, phone mode switches, timeouts.
#include <doctest/doctest.h>

#include "Harness.hpp"

using namespace pf;
using namespace pf::test;
using namespace std::chrono_literals;

TEST_CASE("network loss: session suspended, resumed, data path restored") {
    Harness h;
    auto& c = h.add_client("PC-01");
    REQUIRE(h.wait_connected(c));
    auto sid = c.attach_sync("bench", h.device_on_port(1));
    REQUIRE(sid.ok());
    auto dev = h.vhci().wait_for_device(0x18d1, 0x4ee7, 5s);
    REQUIRE(h.vhci().control_in(dev, 0x80, 0x06, 0x0100, 0, 18).status == UsbStatus::Success);

    c.debug_drop_connection("bench");
    // virtual device stays plugged during the outage
    CHECK(wait_for([&] { return c.session(*sid)->state() != SessionState::Active; }, 3s));
    CHECK(h.vhci().plugged_count() == 1);
    // reconnect + SESSION_RESUME
    REQUIRE(wait_for([&] { return c.session(*sid) && c.session(*sid)->state() == SessionState::Active; }, 10s));
    CHECK(c.servers()[0].reconnects >= 1);
    auto r = h.vhci().control_in(dev, 0x80, 0x06, 0x0100, 0, 18);
    CHECK(r.status == UsbStatus::Success);
    CHECK(h.vhci().plugged_count() == 1);
    CHECK(h.server->status_json()["reconnects"].get<int>() >= 1);
}

TEST_CASE("phone mode switches keep the logical session (ADB -> bootloader -> fastboot/recovery -> EDL -> back)") {
    Harness h;
    auto& c = h.add_client("PC-01");
    REQUIRE(h.wait_connected(c));
    auto sid = c.attach_sync("bench", h.device_on_port(1));
    REQUIRE(sid.ok());
    auto& v = h.vhci();
    auto dev = v.wait_for_device(0x18d1, 0x4ee7, 5s);
    REQUIRE(dev);
    struct Step {
        int mode_index;
        std::uint16_t vid, pid;
        const char* mode_id;
    };
    // indexes into LoopbackBackend::phone_modes()
    const Step steps[] = {{1, 0x18d1, 0x4ee0, "fastboot"},     {2, 0x18d1, 0xd001, "adb"},
                          {3, 0x05c6, 0x9008, "qualcomm-edl"}, {4, 0x0e8d, 0x2000, "mtk-preloader"},
                          {5, 0x0e8d, 0x0003, "mtk-brom"},     {6, 0x04e8, 0x685d, "samsung-download"},
                          {0, 0x18d1, 0x4ee7, "adb"}};
    for (const auto& st : steps) {
        CAPTURE(st.mode_id);
        // what "adb reboot bootloader" etc. does: a vendor request makes the phone re-enumerate
        auto r = v.control_out(dev, 0x40, 0xF0, static_cast<std::uint16_t>(st.mode_index), 0, {});
        CHECK(r.status == UsbStatus::Success);
        const auto next = v.wait_for_device(st.vid, st.pid, 10s);
        REQUIRE_MESSAGE(next != 0, "device did not come back in the new mode");
        CHECK(next != dev);  // re-plugged as a new virtual device
        dev = next;
        auto s = c.session(*sid);
        REQUIRE(s);  // same logical session
        CHECK(wait_for([&] { return s->state() == SessionState::Active; }, 5s));
        CHECK(s->device().mode.id == st.mode_id);
        auto d = v.control_in(dev, 0x80, 0x06, 0x0100, 0, 18);
        REQUIRE(d.status == UsbStatus::Success);
        CHECK(load_le16(&d.data[10]) == st.pid);
        CHECK(h.vhci().plugged_count() == 1);
    }
    CHECK(h.server->status_json()["reenumerations"].get<int>() >= 7);
}

TEST_CASE("a different phone on another port is never bound to the session") {
    Harness h;
    auto& c = h.add_client("PC-01");
    REQUIRE(h.wait_connected(c));
    auto sid = c.attach_sync("bench", h.device_on_port(1));
    REQUIRE(sid.ok());
    h.vhci().wait_for_device(0x18d1, 0x4ee7, 5s);
    // Phone 1 leaves for good; an identical model (different serial, other port) arrives.
    h.backend->remove_device("1-1");
    h.backend->add_device(server::LoopbackBackend::phone_mode_spec("bootloader", "OTHERPHONE1"), "1-7");
    CHECK(wait_for([&] { return c.session(*sid)->state() == SessionState::Reconnecting; }, 5s));
    std::this_thread::sleep_for(500ms);
    CHECK(c.session(*sid)->state() == SessionState::Reconnecting);
    CHECK(h.vhci().plugged_count() == 0);
}

TEST_CASE("session expires when the device never returns") {
    HarnessOptions o;
    o.reconnect_timeout_ms = 600;
    Harness h(o);
    auto& c = h.add_client("PC-01");
    REQUIRE(h.wait_connected(c));
    auto sid = c.attach_sync("bench", h.device_on_port(1));
    REQUIRE(sid.ok());
    h.backend->remove_device("1-1");
    CHECK(wait_for([&] { return c.session(*sid) == nullptr; }, 5s));
    CHECK(h.server->sessions_json().empty());
}

TEST_CASE("client crash: same client re-attaches (takeover) without waiting for the timeout") {
    Harness h;
    std::string client_id;
    std::string id = h.device_on_port(1);
    {
        auto& c = h.add_client("PC-01");
        REQUIRE(h.wait_connected(c));
        client_id = c.config().client_id;
        auto first = c.attach_sync("bench", id);
        REQUIRE_MESSAGE(first.ok(), (first.ok() ? std::string() : first.error().describe()));
        c.debug_drop_connection("bench");
        c.stop();  // process "dies" without detaching
    }
    // restart with the same identity
    ClientConfig cc;
    cc.client_id = client_id;
    cc.client_name = "PC-01";
    ServerEntry e;
    e.name = "bench";
    e.host = "127.0.0.1";
    e.port = h.server->port();
    cc.servers.push_back(e);
    auto vh = std::make_unique<client::TestVhci>();
    auto* vraw = vh.get();
    client::ClientEngine c2(cc, std::move(vh), [&](const ServerEntry&) { return h.token; });
    REQUIRE(c2.start().ok());
    REQUIRE(h.wait_connected(c2));
    auto s = c2.attach_sync("bench", id);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().describe()));
    CHECK(vraw->wait_for_device(0x18d1, 0x4ee7, 5s) != 0);
    c2.stop();
}
