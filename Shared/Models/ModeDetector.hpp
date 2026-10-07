// USB mode detection (ADB, Fastboot, Qualcomm EDL, MTK BROM/Preloader, Samsung
// Download, ...). Purely informational: it labels devices for the UI and feeds Mobile
// Repair Mode heuristics. It never restricts which devices can be redirected.
//
// Signatures are data driven (JSON) so new phones/modes can be added without code
// changes. Built-in defaults live in ModeDetector.cpp; an override file can be loaded.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Common/Error.hpp"
#include "Models/Descriptors.hpp"

namespace pf {

struct DetectedMode {
    std::string id = "generic";  // machine id: "adb", "fastboot", "qualcomm-edl", ...
    std::string label = "USB device";
    std::string family;          // "android", "qualcomm", "mediatek", "samsung", ...
    bool repair_mode = false;    // true for modes typically used for flashing/repair
};

struct ModeSignature {
    std::string id, label, family;
    bool repair = false;
    int priority = 0;  // higher evaluated first
    // match criteria; unset (-1 / empty) criteria are ignored; all set criteria must match
    int vid = -1, pid = -1;
    int if_class = -1, if_subclass = -1, if_protocol = -1;  // any interface must match
    int dev_class = -1;
    std::string product_contains;  // case-insensitive substring of iProduct
};

class ModeDetector {
public:
    ModeDetector();  // loads built-in signatures

    Status load_json(std::string_view json_text, bool replace = false);
    [[nodiscard]] DetectedMode detect(const DescriptorSet& set) const;
    [[nodiscard]] const std::vector<ModeSignature>& signatures() const noexcept { return sigs_; }

    static std::string_view builtin_json();

private:
    std::vector<ModeSignature> sigs_;
};

}  // namespace pf
