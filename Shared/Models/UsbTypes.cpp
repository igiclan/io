#include "Models/UsbTypes.hpp"

namespace pf {

std::string_view to_string(UsbSpeed s) noexcept {
    switch (s) {
        case UsbSpeed::Low: return "low";
        case UsbSpeed::Full: return "full";
        case UsbSpeed::High: return "high";
        case UsbSpeed::Super: return "super";
        case UsbSpeed::SuperPlus: return "super-plus";
        case UsbSpeed::Unknown: break;
    }
    return "unknown";
}

std::string_view to_string(TransferType t) noexcept {
    switch (t) {
        case TransferType::Control: return "control";
        case TransferType::Isochronous: return "isochronous";
        case TransferType::Bulk: return "bulk";
        case TransferType::Interrupt: return "interrupt";
    }
    return "invalid";
}

std::string_view to_string(UsbStatus s) noexcept {
    switch (s) {
        case UsbStatus::Success: return "success";
        case UsbStatus::Stall: return "stall";
        case UsbStatus::Timeout: return "timeout";
        case UsbStatus::Cancelled: return "cancelled";
        case UsbStatus::Overflow: return "overflow";
        case UsbStatus::ShortPacket: return "short-packet";
        case UsbStatus::DeviceGone: return "device-gone";
        case UsbStatus::NoDevice: return "no-device";
        case UsbStatus::InvalidRequest: return "invalid-request";
        case UsbStatus::NotSupported: return "not-supported";
        case UsbStatus::IoError: return "io-error";
        case UsbStatus::Busy: return "busy";
        case UsbStatus::NoResources: return "no-resources";
        case UsbStatus::NotAuthorized: return "not-authorized";
    }
    return "invalid";
}

std::string_view to_string(AccessMode m) noexcept {
    return m == AccessMode::SharedReadOnly ? "shared-readonly" : "exclusive";
}

std::string_view to_string(SessionPriority p) noexcept {
    switch (p) {
        case SessionPriority::Low: return "low";
        case SessionPriority::Normal: return "normal";
        case SessionPriority::High: return "high";
        case SessionPriority::Critical: return "critical";
    }
    return "normal";
}

bool parse_speed(std::string_view s, UsbSpeed& out) noexcept {
    for (auto v : {UsbSpeed::Unknown, UsbSpeed::Low, UsbSpeed::Full, UsbSpeed::High, UsbSpeed::Super,
                   UsbSpeed::SuperPlus}) {
        if (to_string(v) == s) { out = v; return true; }
    }
    return false;
}

bool parse_access_mode(std::string_view s, AccessMode& out) noexcept {
    if (s == "exclusive") { out = AccessMode::Exclusive; return true; }
    if (s == "shared-readonly" || s == "shared") { out = AccessMode::SharedReadOnly; return true; }
    return false;
}

bool parse_priority(std::string_view s, SessionPriority& out) noexcept {
    for (auto v : {SessionPriority::Low, SessionPriority::Normal, SessionPriority::High, SessionPriority::Critical}) {
        if (to_string(v) == s) { out = v; return true; }
    }
    return false;
}

std::uint64_t speed_bits_per_second(UsbSpeed s) noexcept {
    switch (s) {
        case UsbSpeed::Low: return 1'500'000ULL;
        case UsbSpeed::Full: return 12'000'000ULL;
        case UsbSpeed::High: return 480'000'000ULL;
        case UsbSpeed::Super: return 5'000'000'000ULL;
        case UsbSpeed::SuperPlus: return 10'000'000'000ULL;
        case UsbSpeed::Unknown: break;
    }
    return 0;
}

}  // namespace pf
