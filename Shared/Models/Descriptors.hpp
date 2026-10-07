// USB descriptor storage and parsing (USB 2.0 ch. 9, USB 3.2 ch. 9).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Common/ByteIo.hpp"
#include "Models/UsbTypes.hpp"

namespace pf {

namespace usb_desc {
inline constexpr std::uint8_t kDevice = 0x01;
inline constexpr std::uint8_t kConfiguration = 0x02;
inline constexpr std::uint8_t kString = 0x03;
inline constexpr std::uint8_t kInterface = 0x04;
inline constexpr std::uint8_t kEndpoint = 0x05;
inline constexpr std::uint8_t kDeviceQualifier = 0x06;
inline constexpr std::uint8_t kInterfaceAssociation = 0x0B;
inline constexpr std::uint8_t kBos = 0x0F;
inline constexpr std::uint8_t kSsEndpointCompanion = 0x30;
}  // namespace usb_desc

struct DeviceDescriptor {
    std::uint16_t bcdUSB = 0;
    std::uint8_t bDeviceClass = 0;
    std::uint8_t bDeviceSubClass = 0;
    std::uint8_t bDeviceProtocol = 0;
    std::uint8_t bMaxPacketSize0 = 0;
    std::uint16_t idVendor = 0;
    std::uint16_t idProduct = 0;
    std::uint16_t bcdDevice = 0;
    std::uint8_t iManufacturer = 0;
    std::uint8_t iProduct = 0;
    std::uint8_t iSerialNumber = 0;
    std::uint8_t bNumConfigurations = 0;
};

struct EndpointInfo {
    std::uint8_t address = 0;      // bEndpointAddress
    std::uint8_t attributes = 0;   // bmAttributes
    std::uint16_t max_packet = 0;  // wMaxPacketSize (incl. high-bandwidth mult bits)
    std::uint8_t interval = 0;
    [[nodiscard]] TransferType type() const noexcept { return static_cast<TransferType>(attributes & 3); }
    [[nodiscard]] bool is_in() const noexcept { return (address & 0x80) != 0; }
};

struct InterfaceInfo {
    std::uint8_t number = 0;
    std::uint8_t alt_setting = 0;
    std::uint8_t cls = 0;
    std::uint8_t subclass = 0;
    std::uint8_t protocol = 0;
    std::uint8_t iInterface = 0;
    std::vector<EndpointInfo> endpoints;
};

struct ConfigurationInfo {
    std::uint8_t value = 0;  // bConfigurationValue
    std::uint8_t attributes = 0;
    std::uint16_t max_power_ma = 0;
    std::uint8_t num_interfaces = 0;
    std::vector<InterfaceInfo> interfaces;  // every alternate setting is a separate entry
};

struct StringDescriptor {
    std::uint8_t index = 0;
    std::uint16_t lang_id = 0;
    Bytes raw;  // full descriptor incl. bLength/bDescriptorType
};

// Everything the client needs to instantiate a virtual device (UDECXUSBDEVICE).
struct DescriptorSet {
    Bytes device;                // 18 bytes
    std::vector<Bytes> configs;  // full configuration descriptors (wTotalLength bytes)
    Bytes bos;                   // optional (USB 2.01+ / 3.x)
    Bytes device_qualifier;      // optional (high-speed capable devices)
    std::vector<StringDescriptor> strings;  // index 0 (language table) first when present
};

std::optional<DeviceDescriptor> parse_device_descriptor(ByteSpan raw);
std::optional<ConfigurationInfo> parse_configuration(ByteSpan raw);

// Decodes a UTF-16LE string descriptor to UTF-8. Returns empty for invalid input.
std::string decode_string_descriptor(ByteSpan raw);
// Builds a string descriptor from UTF-8 (BMP only), used by test devices.
Bytes make_string_descriptor(std::string_view utf8);

// Returns the UTF-8 string for the given index (first language) or "".
std::string find_string(const DescriptorSet& set, std::uint8_t index);

// Basic structural validation used before handing descriptors to the kernel.
bool validate_descriptor_set(const DescriptorSet& set, std::string* why = nullptr);

std::string format_bcd(std::uint16_t bcd);  // 0x0210 -> "2.10"

}  // namespace pf
