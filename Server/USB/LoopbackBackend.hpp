// TEST DOUBLE. Emulated USB devices for automated tests and benchmarks.
//
// Never presented as real hardware: the server only creates it with
// usb.backend = "test" (or --test-devices), and the UI labels such devices "TEST".
//
// Each emulated device implements:
//   EP0       standard GET_DESCRIPTOR / GET_STATUS / GET_CONFIGURATION / SET_*,
//             vendor 0x40/0x02 store, 0xC0/0x03 read back, 0xC0/0x01 counter,
//             vendor 0x40/0xF0 wValue=<mode index> -> re-enumerate in another mode
//             (simulates "adb reboot bootloader", "reboot edl", ...)
//   0x01 OUT  bulk -> loopback FIFO        0x81 IN bulk <- FIFO (pends until data)
//   0x03 OUT  bulk sink (discard)          0x84 IN bulk source (zeros, immediate)
//   0x82 IN   interrupt counter (8 bytes)  0x85 IN isochronous pattern
#pragma once

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Models/DescriptorBuilder.hpp"
#include "Network/Transport.hpp"
#include "USB/UsbBackend.hpp"

namespace pf::server {

class LoopbackBackend final : public IUsbBackend {
public:
    // Modes understood by phone_mode_spec / switch_mode, in vendor-request index order.
    static const std::vector<std::string>& phone_modes();
    static DeviceSpec phone_mode_spec(const std::string& mode, const std::string& serial);

    explicit LoopbackBackend(std::uint32_t initial_devices);
    ~LoopbackBackend() override;

    std::string name() const override { return "test-loopback"; }
    Status start() override;
    void stop() override;
    Result<std::vector<EnumeratedDevice>> enumerate() override;
    Result<std::unique_ptr<IUsbDevice>> open(const DeviceIdentity& identity) override;
    void set_change_callback(std::function<void()> cb) override;
    bool has_change_notifications() const override { return true; }

    // ---- test controls (thread-safe) ----
    void add_device(const DeviceSpec& spec, const std::string& port_path);
    void remove_device(const std::string& port_path);
    // Removes the device and re-adds it in `mode` after delay_ms (same port, same unit).
    void switch_mode(const std::string& port_path, const std::string& mode, std::uint32_t delay_ms = 20);
    void set_latency_us(std::uint32_t us) { latency_us_ = us; }
    void set_fail_open(bool f) { fail_open_ = f; }
    [[nodiscard]] std::size_t present_count() const;
    [[nodiscard]] std::size_t open_count() const;

    struct VirtualDevice;

private:
    friend class LoopbackDevice;
    void notify_changed();

    mutable std::mutex m_;
    std::map<std::string, std::shared_ptr<VirtualDevice>> devices_;  // by port path
    std::map<std::string, std::string> unit_serial_;                  // port path -> unit serial
    std::function<void()> on_change_;
    std::unique_ptr<net::IoRuntime> rt_;
    std::atomic<std::uint32_t> latency_us_{0};
    std::atomic<bool> fail_open_{false};
    std::atomic<std::size_t> open_count_{0};
    std::uint8_t next_address_ = 1;
};

}  // namespace pf::server
