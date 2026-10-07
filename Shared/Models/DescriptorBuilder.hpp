// Programmatic construction of USB descriptors. Used by the loopback test device and
// by unit tests; not used on the production data path.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Models/Descriptors.hpp"

namespace pf {

struct EndpointSpec {
    std::uint8_t address;
    TransferType type;
    std::uint16_t max_packet;
    std::uint8_t interval = 0;
};

struct InterfaceSpec {
    std::uint8_t number = 0;
    std::uint8_t alt = 0;
    std::uint8_t cls = 0xFF, subclass = 0, protocol = 0;
    std::string name;  // becomes iInterface string when non-empty
    std::vector<EndpointSpec> endpoints;
};

struct DeviceSpec {
    std::uint16_t vid = 0x1209;  // pid.codes test VID
    std::uint16_t pid = 0x0001;
    std::uint16_t bcd_usb = 0x0200;
    std::uint16_t bcd_device = 0x0100;
    std::uint8_t dev_class = 0, dev_subclass = 0, dev_protocol = 0;
    std::uint8_t max_packet0 = 64;
    std::string manufacturer = "ParsFlash";
    std::string product = "Test Device";
    std::string serial;
    std::vector<InterfaceSpec> interfaces;
    UsbSpeed speed = UsbSpeed::High;
};

// Builds a complete DescriptorSet (device, single configuration, strings with
// language table 0x0409).
DescriptorSet build_descriptor_set(const DeviceSpec& spec);

}  // namespace pf
