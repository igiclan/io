// Device description exchanged between server, client, API and UI.
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "Common/Error.hpp"
#include "Models/Descriptors.hpp"
#include "Models/DeviceIdentity.hpp"
#include "Models/ModeDetector.hpp"

namespace pf {

enum class DeviceShareState : std::uint8_t {
    Available = 0,   // present, not shared
    Shared = 1,      // offered to clients, nobody attached
    InUse = 2,       // attached by a client
    Unavailable = 3, // cannot be captured (driver/permission problem) - see access_error
};
std::string_view to_string(DeviceShareState s) noexcept;
bool parse_share_state(std::string_view s, DeviceShareState& out) noexcept;

struct DeviceInfo {
    std::string id;
    DeviceIdentity identity;
    std::string manufacturer;
    std::string product;
    std::uint16_t bcd_usb = 0;
    std::uint16_t bcd_device = 0;
    std::uint8_t dev_class = 0, dev_subclass = 0, dev_protocol = 0;
    DetectedMode mode;
    DeviceShareState state = DeviceShareState::Available;
    bool shared = false;
    std::string owner;       // owning client id
    std::string owner_name;  // owning client display name
    std::string session_id;
    std::string access_error;
    std::string driver;      // function driver on the server (Windows), informational

    [[nodiscard]] std::string display_name() const;
};

// Fills descriptor-derived fields (vid/pid/strings/mode) from a descriptor set.
void fill_from_descriptors(DeviceInfo& info, const DescriptorSet& set, const ModeDetector& detector);

nlohmann::json to_json(const DeviceInfo& d);
Result<DeviceInfo> device_info_from_json(const nlohmann::json& j);
nlohmann::json to_json(const DescriptorSet& s);
Result<DescriptorSet> descriptor_set_from_json(const nlohmann::json& j);

std::string format_vid_pid(std::uint16_t vid, std::uint16_t pid);  // "18d1:4ee7"

}  // namespace pf
