// TEST DOUBLE for pfvhci.sys. Plays the kernel driver *and* a USB function driver:
// tests call submit_urb() the way Windows' USB stack would hand a URB to the driver.
// Every request and completion is round-tripped through the real ABI encoders and
// decoders (VhciRecords), so the user-mode side of the driver contract is verified.
// It does NOT expose anything to the operating system.
#pragma once

#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include "VirtualUSB/VirtualHci.hpp"

namespace pf::client {

struct UrbResult {
    UsbStatus status = UsbStatus::Success;
    std::uint32_t actual_length = 0;
    Bytes data;  // IN data
    std::vector<proto::IsoResult> iso;
};

struct TestUrb {
    std::uint8_t endpoint = 0;
    TransferType type = TransferType::Bulk;
    std::uint16_t flags = 0;
    std::uint32_t length = 0;  // IN length
    std::array<std::uint8_t, 8> setup{};
    Bytes out;                 // OUT data
    std::vector<proto::IsoPacket> iso;
};

struct PlugEvent {
    bool plugged;
    std::uint32_t handle;
    std::uint64_t cookie;
    DescriptorSet descriptors;
    UsbSpeed speed;
};

class TestVhci final : public IVirtualHci {
public:
    TestVhci();
    ~TestVhci() override;

    Status open() override;
    void close() override;
    VhciInfo info() const override;
    void set_sink(RequestSink sink) override { sink_ = std::move(sink); }
    Result<PluggedDevice> plug(const DescriptorSet& set, UsbSpeed speed, std::uint64_t cookie) override;
    Status unplug(std::uint32_t handle) override;
    Status complete(const VhciCompletion& c) override;

    // ---- "function driver" side ----
    std::future<UrbResult> submit_urb(std::uint32_t handle, const TestUrb& urb, std::uint64_t* request_id = nullptr);
    // OS cancels a URB (e.g. application closed): completes it as Cancelled and emits CANCEL.
    void cancel_urb(std::uint32_t handle, std::uint64_t request_id);
    std::future<UsbStatus> select_interface(std::uint32_t handle, std::uint8_t iface, std::uint8_t alt);
    std::future<UsbStatus> select_configuration(std::uint32_t handle, std::uint8_t value);
    std::future<UsbStatus> reset_endpoint(std::uint32_t handle, std::uint8_t ep);

    // Convenience synchronous helpers (timeout -> status Timeout).
    UrbResult control_in(std::uint32_t handle, std::uint8_t bm, std::uint8_t req, std::uint16_t value,
                         std::uint16_t index, std::uint16_t length);
    UrbResult control_out(std::uint32_t handle, std::uint8_t bm, std::uint8_t req, std::uint16_t value,
                          std::uint16_t index, const Bytes& data);
    UrbResult bulk_out(std::uint32_t handle, std::uint8_t ep, const Bytes& data);
    UrbResult bulk_in(std::uint32_t handle, std::uint8_t ep, std::uint32_t length);

    // Observability
    [[nodiscard]] std::vector<PlugEvent> events() const;
    [[nodiscard]] std::size_t plugged_count() const;
    // Waits until a device is plugged whose descriptors have the given VID:PID; returns its handle (0 on timeout).
    std::uint32_t wait_for_device(std::uint16_t vid, std::uint16_t pid, std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t pending_urbs() const;

private:
    struct Device {
        std::uint64_t cookie;
        DescriptorSet desc;
        UsbSpeed speed;
        std::uint32_t port;
    };
    struct Pending {
        std::uint32_t handle;
        bool in;
        bool op;
        std::promise<UrbResult> urb;
        std::promise<UsbStatus> opp;
    };
    void deliver(Bytes record);
    void run();
    std::future<UsbStatus> op(std::uint32_t handle, VhciRecordKind kind, std::uint8_t a, std::uint8_t b,
                              std::uint8_t ep);

    RequestSink sink_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::map<std::uint32_t, Device> devices_;
    std::map<std::uint64_t, Pending> pending_;
    std::vector<PlugEvent> events_;
    std::deque<Bytes> queue_;  // encoded FETCH records awaiting delivery
    std::uint32_t next_handle_ = 1;
    std::uint64_t next_request_ = 1;
    bool running_ = false;
    std::thread thread_;
};

}  // namespace pf::client
