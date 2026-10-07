#include "Models/DeviceInfo.hpp"

#include <cstdio>
#include <nlohmann/json.hpp>

namespace pf {

using nlohmann::json;

std::string_view to_string(DeviceShareState s) noexcept {
    switch (s) {
        case DeviceShareState::Available: return "available";
        case DeviceShareState::Shared: return "shared";
        case DeviceShareState::InUse: return "in-use";
        case DeviceShareState::Unavailable: return "unavailable";
    }
    return "available";
}

bool parse_share_state(std::string_view s, DeviceShareState& out) noexcept {
    for (auto v : {DeviceShareState::Available, DeviceShareState::Shared, DeviceShareState::InUse,
                   DeviceShareState::Unavailable}) {
        if (to_string(v) == s) {
            out = v;
            return true;
        }
    }
    return false;
}

std::string format_vid_pid(std::uint16_t vid, std::uint16_t pid) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04x:%04x", vid, pid);
    return buf;
}

std::string DeviceInfo::display_name() const {
    std::string n = product.empty() ? std::string("USB device ") + format_vid_pid(identity.vid, identity.pid) : product;
    if (!manufacturer.empty() && product.find(manufacturer) == std::string::npos) n = manufacturer + " " + n;
    return n;
}

void fill_from_descriptors(DeviceInfo& info, const DescriptorSet& set, const ModeDetector& detector) {
    if (auto dd = parse_device_descriptor(set.device)) {
        info.identity.vid = dd->idVendor;
        info.identity.pid = dd->idProduct;
        info.bcd_usb = dd->bcdUSB;
        info.bcd_device = dd->bcdDevice;
        info.dev_class = dd->bDeviceClass;
        info.dev_subclass = dd->bDeviceSubClass;
        info.dev_protocol = dd->bDeviceProtocol;
        info.manufacturer = find_string(set, dd->iManufacturer);
        info.product = find_string(set, dd->iProduct);
        const std::string serial = find_string(set, dd->iSerialNumber);
        if (!serial.empty()) info.identity.serial = serial;
    }
    info.mode = detector.detect(set);
    std::uint64_t h = fnv1a64(set.device.data(), set.device.size());
    for (const auto& c : set.configs) h = fnv1a64(c.data(), c.size(), h);
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    info.identity.descriptor_hash = buf;
}

json to_json(const DeviceInfo& d) {
    return {{"id", d.id},
            {"vid", d.identity.vid},
            {"pid", d.identity.pid},
            {"vid_pid", format_vid_pid(d.identity.vid, d.identity.pid)},
            {"serial", d.identity.serial},
            {"manufacturer", d.manufacturer},
            {"product", d.product},
            {"name", d.display_name()},
            {"bcd_usb", d.bcd_usb},
            {"usb_version", format_bcd(d.bcd_usb)},
            {"bcd_device", d.bcd_device},
            {"class", d.dev_class},
            {"subclass", d.dev_subclass},
            {"protocol", d.dev_protocol},
            {"speed", to_string(d.identity.speed)},
            {"bus", d.identity.bus},
            {"address", d.identity.address},
            {"port", d.identity.port},
            {"port_path", d.identity.port_path},
            {"location", d.identity.location},
            {"parent", d.identity.parent_id},
            {"container_id", d.identity.container_id},
            {"instance_id", d.identity.instance_id},
            {"descriptor_hash", d.identity.descriptor_hash},
            {"mode", d.mode.id},
            {"mode_label", d.mode.label},
            {"mode_family", d.mode.family},
            {"repair_mode", d.mode.repair_mode},
            {"state", to_string(d.state)},
            {"shared", d.shared},
            {"owner", d.owner},
            {"owner_name", d.owner_name},
            {"session_id", d.session_id},
            {"access_error", d.access_error},
            {"driver", d.driver}};
}

Result<DeviceInfo> device_info_from_json(const json& j) {
    if (!j.is_object()) return Error{ErrorCode::MalformedPayload, "device must be an object"};
    DeviceInfo d;
    try {
        d.id = j.at("id").get<std::string>();
        d.identity.vid = j.value("vid", std::uint16_t{0});
        d.identity.pid = j.value("pid", std::uint16_t{0});
        d.identity.serial = j.value("serial", "");
        d.manufacturer = j.value("manufacturer", "");
        d.product = j.value("product", "");
        d.bcd_usb = j.value("bcd_usb", std::uint16_t{0});
        d.bcd_device = j.value("bcd_device", std::uint16_t{0});
        d.dev_class = j.value("class", std::uint8_t{0});
        d.dev_subclass = j.value("subclass", std::uint8_t{0});
        d.dev_protocol = j.value("protocol", std::uint8_t{0});
        parse_speed(j.value("speed", "unknown"), d.identity.speed);
        d.identity.bus = j.value("bus", std::uint8_t{0});
        d.identity.address = j.value("address", std::uint8_t{0});
        d.identity.port = j.value("port", std::uint8_t{0});
        d.identity.port_path = j.value("port_path", "");
        d.identity.location = j.value("location", "");
        d.identity.parent_id = j.value("parent", "");
        d.identity.container_id = j.value("container_id", "");
        d.identity.instance_id = j.value("instance_id", "");
        d.identity.descriptor_hash = j.value("descriptor_hash", "");
        d.mode.id = j.value("mode", "generic");
        d.mode.label = j.value("mode_label", "USB device");
        d.mode.family = j.value("mode_family", "");
        d.mode.repair_mode = j.value("repair_mode", false);
        parse_share_state(j.value("state", "available"), d.state);
        d.shared = j.value("shared", false);
        d.owner = j.value("owner", "");
        d.owner_name = j.value("owner_name", "");
        d.session_id = j.value("session_id", "");
        d.access_error = j.value("access_error", "");
        d.driver = j.value("driver", "");
    } catch (const json::exception& e) {
        return Error{ErrorCode::MalformedPayload, std::string("device: ") + e.what()};
    }
    return d;
}

json to_json(const DescriptorSet& s) {
    json configs = json::array();
    for (const auto& c : s.configs) configs.push_back(base64_encode(c));
    json strings = json::array();
    for (const auto& st : s.strings)
        strings.push_back({{"index", st.index}, {"lang", st.lang_id}, {"data", base64_encode(st.raw)}});
    return {{"device", base64_encode(s.device)},
            {"configs", configs},
            {"bos", base64_encode(s.bos)},
            {"qualifier", base64_encode(s.device_qualifier)},
            {"strings", strings}};
}

Result<DescriptorSet> descriptor_set_from_json(const json& j) {
    DescriptorSet s;
    auto b64 = [](const json& v, Bytes& out) {
        return v.is_string() && base64_decode(v.get<std::string>(), out);
    };
    if (!j.is_object() || !j.contains("device") || !b64(j["device"], s.device))
        return Error{ErrorCode::MalformedPayload, "descriptors.device"};
    if (j.contains("configs") && j["configs"].is_array()) {
        for (const auto& c : j["configs"]) {
            Bytes b;
            if (!b64(c, b)) return Error{ErrorCode::MalformedPayload, "descriptors.configs"};
            s.configs.push_back(std::move(b));
        }
    }
    if (j.contains("bos") && !b64(j["bos"], s.bos)) return Error{ErrorCode::MalformedPayload, "descriptors.bos"};
    if (j.contains("qualifier") && !b64(j["qualifier"], s.device_qualifier))
        return Error{ErrorCode::MalformedPayload, "descriptors.qualifier"};
    if (j.contains("strings") && j["strings"].is_array()) {
        for (const auto& st : j["strings"]) {
            StringDescriptor sd;
            if (!st.is_object() || !st.contains("data") || !b64(st["data"], sd.raw))
                return Error{ErrorCode::MalformedPayload, "descriptors.strings"};
            sd.index = st.value("index", std::uint8_t{0});
            sd.lang_id = st.value("lang", std::uint16_t{0});
            s.strings.push_back(std::move(sd));
        }
    }
    std::string why;
    if (!validate_descriptor_set(s, &why)) return Error{ErrorCode::MalformedPayload, why};
    return s;
}

}  // namespace pf
