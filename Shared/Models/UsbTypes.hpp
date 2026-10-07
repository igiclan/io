// USB enumerations shared by server, client, protocol and driver bridge.
#pragma once

#include <cstdint>
#include <string_view>

namespace pf {

enum class UsbSpeed : std::uint8_t {
    Unknown = 0,
    Low = 1,        // 1.5 Mbit/s
    Full = 2,       // 12 Mbit/s
    High = 3,       // 480 Mbit/s
    Super = 4,      // 5 Gbit/s
    SuperPlus = 5,  // 10 Gbit/s+
};

enum class TransferType : std::uint8_t {
    Control = 0,
    Isochronous = 1,
    Bulk = 2,
    Interrupt = 3,
};

// Completion status of a USB transfer as carried by USB_COMPLETE.
// Values are part of PFUP/1; append only.
enum class UsbStatus : std::int32_t {
    Success = 0,
    Stall = 1,           // endpoint halted / protocol stall on EP0
    Timeout = 2,
    Cancelled = 3,
    Overflow = 4,        // device sent more than requested (babble)
    ShortPacket = 5,     // short transfer with SHORT_NOT_OK set
    DeviceGone = 6,      // device disconnected
    NoDevice = 7,        // unknown device handle
    InvalidRequest = 8,  // malformed submit (bad endpoint, length)
    NotSupported = 9,    // e.g. isochronous on a backend that cannot do it
    IoError = 10,        // CRC / bit-stuff / generic transport error
    Busy = 11,
    NoResources = 12,
    NotAuthorized = 13,  // read-only/shared mode tried to submit
};

// USB_SUBMIT xfer_flags
namespace xfer_flags {
inline constexpr std::uint16_t kShortNotOk = 0x0001;
inline constexpr std::uint16_t kZeroPacket = 0x0002;  // terminate OUT with ZLP when multiple of wMaxPacketSize
inline constexpr std::uint16_t kDirIn = 0x0004;
}  // namespace xfer_flags

enum class AccessMode : std::uint8_t { Exclusive = 0, SharedReadOnly = 1 };

enum class SessionPriority : std::uint8_t { Low = 0, Normal = 1, High = 2, Critical = 3 };

std::string_view to_string(UsbSpeed s) noexcept;
std::string_view to_string(TransferType t) noexcept;
std::string_view to_string(UsbStatus s) noexcept;
std::string_view to_string(AccessMode m) noexcept;
std::string_view to_string(SessionPriority p) noexcept;

bool parse_speed(std::string_view s, UsbSpeed& out) noexcept;
bool parse_access_mode(std::string_view s, AccessMode& out) noexcept;
bool parse_priority(std::string_view s, SessionPriority& out) noexcept;

// Nominal signalling rate in bits/s (for UI).
std::uint64_t speed_bits_per_second(UsbSpeed s) noexcept;

inline bool endpoint_is_in(std::uint8_t ep) noexcept { return (ep & 0x80) != 0; }

}  // namespace pf
