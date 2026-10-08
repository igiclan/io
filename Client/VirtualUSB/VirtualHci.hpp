// Client-side virtual USB host controller.
//
// WinVhci  : talks to the pfvhci.sys UdeCx driver (Windows). Devices plugged here
//            appear in Device Manager and are used by normal Windows applications.
// TestVhci : TEST DOUBLE playing the role of the kernel driver + a function driver,
//            for automated tests and benchmarks (Tests/, Tools/pfbench).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "Common/Config.hpp"
#include "Common/Error.hpp"
#include "VirtualUSB/VhciRecords.hpp"

namespace pf::client {

struct VhciInfo {
    std::uint32_t abi_version = 0;
    std::string driver_version;
    std::uint32_t usb2_ports = 0;
    std::uint32_t usb3_ports = 0;
    bool is_test_double = false;
};

struct PluggedDevice {
    std::uint32_t handle = 0;
    std::uint32_t port = 0;
};

class IVirtualHci {
public:
    // Called on a fetch thread for every record. Spans in the request are only valid
    // during the call.
    using RequestSink = std::function<void(const VhciRequest&)>;

    virtual ~IVirtualHci() = default;
    virtual Status open() = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual VhciInfo info() const = 0;
    virtual void set_sink(RequestSink sink) = 0;  // before open()
    virtual Result<PluggedDevice> plug(const DescriptorSet& set, UsbSpeed speed, std::uint64_t cookie) = 0;
    virtual Status unplug(std::uint32_t handle) = 0;
    virtual Status complete(const VhciCompletion& c) = 0;
};

#ifdef _WIN32
std::unique_ptr<IVirtualHci> make_windows_vhci(const VhciConfig& cfg);
#endif

}  // namespace pf::client
