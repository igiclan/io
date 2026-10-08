// Server-side USB access abstraction.
//
// IUsbBackend enumerates devices and opens ("captures") them; IUsbDevice executes
// transfers asynchronously. Implementations:
//   LibusbBackend   - production (Windows: libusb + UsbDk/WinUSB, Linux: usbfs)
//   LoopbackBackend - emulated devices for automated tests and benchmarks only
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Common/BufferPool.hpp"
#include "Common/Error.hpp"
#include "Models/Descriptors.hpp"
#include "Models/DeviceIdentity.hpp"
#include "Protocol/Protocol.hpp"

namespace pf::server {

// One USB transfer. Buffers are arranged so that no copy is needed on the data path:
//   OUT: buffer holds the received frame payload; data at [data_offset, +length)
//   IN : buffer was allocated with proto::complete_prefix_size() in front of the data
//        region so the completion frame can be finalized in place.
//   Control: the 8 setup bytes are located at [data_offset - 8, data_offset).
struct TransferRequest {
    std::uint64_t seq = 0;
    std::uint8_t endpoint = 0;
    TransferType type = TransferType::Bulk;
    std::uint16_t flags = 0;
    std::uint32_t timeout_ms = 0;
    std::uint32_t length = 0;
    std::array<std::uint8_t, 8> setup{};
    std::vector<proto::IsoPacket> iso;
    PooledBuffer buffer;
    std::size_t data_offset = 0;
    [[nodiscard]] bool is_in() const noexcept { return (flags & xfer_flags::kDirIn) != 0; }
    [[nodiscard]] std::uint8_t* data() noexcept { return buffer.data() + data_offset; }
};

struct TransferResult {
    std::uint64_t seq = 0;
    UsbStatus status = UsbStatus::Success;
    std::uint32_t actual_length = 0;
    std::uint32_t iso_error_count = 0;
    std::vector<proto::IsoResult> iso;
    PooledBuffer buffer;          // the request buffer handed back
    std::size_t data_offset = 0;  // IN data location inside buffer
};

using TransferCallback = std::function<void(TransferResult&&)>;

class IUsbDevice {
public:
    virtual ~IUsbDevice() = default;
    // Asynchronous. `done` is invoked exactly once, on a backend thread.
    virtual void submit(TransferRequest&& req, TransferCallback done) = 0;
    virtual void cancel(std::uint64_t seq) = 0;
    virtual void cancel_all() = 0;
    // Blocking operations: call from a blocking-work executor, never from I/O threads.
    virtual Status reset_port() = 0;
    virtual Status set_configuration(std::uint8_t value) = 0;
    virtual Status set_interface(std::uint8_t iface, std::uint8_t alt) = 0;
    virtual Status clear_halt(std::uint8_t endpoint) = 0;
    // Waits for outstanding transfers, releases the device back to the OS.
    virtual void close() = 0;
    // Exact descriptors read from the device after capture.
    [[nodiscard]] virtual const DescriptorSet& descriptors() const = 0;
    [[nodiscard]] virtual UsbSpeed speed() const = 0;
};

// A device as seen during enumeration (before capture).
struct EnumeratedDevice {
    DeviceIdentity identity;
    DescriptorSet descriptors;  // may lack strings when they cannot be read without capture
    std::string manufacturer, product;  // from the OS when available
    std::string driver;
    std::string access_error;   // non-empty when capture is known to be impossible
};

class IUsbBackend {
public:
    virtual ~IUsbBackend() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    virtual Status start() = 0;
    virtual void stop() = 0;
    virtual Result<std::vector<EnumeratedDevice>> enumerate() = 0;
    // Captures the device identified by `identity` (port path / instance id + vid/pid).
    virtual Result<std::unique_ptr<IUsbDevice>> open(const DeviceIdentity& identity) = 0;
    // Called (from any thread) when the backend notices topology changes; the engine
    // then rescans. Backends without notifications rely on periodic polling.
    virtual void set_change_callback(std::function<void()> cb) = 0;
    [[nodiscard]] virtual bool has_change_notifications() const = 0;
};

// Maps standard control requests that must go through the host API instead of a raw
// control transfer (SET_CONFIGURATION, SET_INTERFACE, CLEAR_FEATURE(ENDPOINT_HALT)).
enum class SpecialControl { None, SetConfiguration, SetInterface, ClearHalt };
SpecialControl classify_control(const std::array<std::uint8_t, 8>& setup) noexcept;

std::unique_ptr<IUsbBackend> make_loopback_backend(std::uint32_t initial_devices);
#ifdef PF_HAVE_LIBUSB
std::unique_ptr<IUsbBackend> make_libusb_backend(bool use_usbdk);
#endif

}  // namespace pf::server
