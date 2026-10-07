// Physical device identity and the matcher used by Smart USB Session Persistence.
//
// A phone that switches mode (ADB -> bootloader, normal -> EDL, preloader -> BROM)
// disconnects and re-enumerates, usually with a different PID and sometimes a
// different/no serial number. The matcher decides whether a newly arrived device is
// the same physical unit as one that recently left a session. VID/PID alone is never
// sufficient: two identical phones on one hub must not be confused.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Models/UsbTypes.hpp"

namespace pf {

struct DeviceIdentity {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::string serial;        // iSerialNumber string ("" when none)
    std::string port_path;     // physical port chain, e.g. "1-4.2" (bus-port.port...)
    std::string location;      // OS location path (Windows LocationPaths) - informational
    std::string parent_id;     // parent hub instance id / sysfs path
    std::string container_id;  // Windows ContainerId GUID ("" when unknown)
    std::string instance_id;   // OS device instance id / device path
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
    std::uint8_t port = 0;     // port number on the parent hub
    UsbSpeed speed = UsbSpeed::Unknown;
    std::string descriptor_hash;  // hash of device + configuration descriptors
};

// Serials such as "0123456789ABCDEF" are shared by thousands of cheap/boot-mode
// devices and must not be treated as unique.
bool is_placeholder_serial(const std::string& serial);

struct IdentityMatchPolicy {
    int threshold = 40;              // minimum score to bind to a previous session
    int serial_equal = 60;
    int serial_conflict = -100;
    int port_equal = 40;
    int port_conflict = -30;
    int parent_equal = 10;
    int container_equal = 20;
    int vid_equal = 5;
    int recent_bonus = 10;           // removed less than recent_ms ago
    std::int64_t recent_ms = 10000;
    std::int64_t max_age_ms = 60000; // beyond this a removed device is never matched
};

struct IdentityMatch {
    int score = 0;
    bool matched = false;
    std::vector<std::string> reasons;
};

class IdentityMatcher {
public:
    explicit IdentityMatcher(IdentityMatchPolicy policy = {}) : policy_(policy) {}
    IdentityMatch match(const DeviceIdentity& previous, std::int64_t removed_at_ms, const DeviceIdentity& candidate,
                        std::int64_t now_ms) const;
    [[nodiscard]] const IdentityMatchPolicy& policy() const noexcept { return policy_; }

private:
    IdentityMatchPolicy policy_;
};

// Stable short identifier for UI/API, derived from the physical location when
// available (so the same port keeps the same id across re-plugs of the same device).
std::string make_device_id(const DeviceIdentity& id);

std::uint64_t fnv1a64(const void* data, std::size_t n, std::uint64_t seed = 1469598103934665603ULL) noexcept;

}  // namespace pf
