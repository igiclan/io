#include "Models/ModeDetector.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace pf {

// Built-in signature table. Interface-class rules (generic, VID independent) have a
// lower priority than exact VID:PID rules. Sources: Android platform/system/core
// (adb/fastboot interface triplets), vendor driver INFs.
static constexpr std::string_view kBuiltinSignatures = R"json({
  "version": 1,
  "signatures": [
    {"id":"qualcomm-edl","label":"Qualcomm EDL (9008)","family":"qualcomm","repair":true,"priority":100,"vid":"05c6","pid":"9008"},
    {"id":"qualcomm-diag","label":"Qualcomm Diagnostics (9006)","family":"qualcomm","repair":true,"priority":100,"vid":"05c6","pid":"9006"},
    {"id":"qualcomm-diag","label":"Qualcomm Diagnostics (901d)","family":"qualcomm","repair":true,"priority":100,"vid":"05c6","pid":"901d"},
    {"id":"mtk-brom","label":"MediaTek BROM","family":"mediatek","repair":true,"priority":100,"vid":"0e8d","pid":"0003"},
    {"id":"mtk-preloader","label":"MediaTek Preloader","family":"mediatek","repair":true,"priority":100,"vid":"0e8d","pid":"2000"},
    {"id":"mtk-da","label":"MediaTek DA (2001)","family":"mediatek","repair":true,"priority":100,"vid":"0e8d","pid":"2001"},
    {"id":"samsung-download","label":"Samsung Download Mode","family":"samsung","repair":true,"priority":100,"vid":"04e8","pid":"685d"},
    {"id":"unisoc-download","label":"Unisoc/Spreadtrum Download","family":"unisoc","repair":true,"priority":100,"vid":"1782","pid":"4d00"},
    {"id":"huawei-usb-com","label":"Huawei USB COM 1.0","family":"huawei","repair":true,"priority":100,"vid":"12d1","pid":"3609"},
    {"id":"fastboot","label":"Fastboot","family":"android","repair":true,"priority":90,"vid":"18d1","pid":"d00d"},
    {"id":"fastboot","label":"Fastboot","family":"android","repair":true,"priority":90,"vid":"18d1","pid":"4ee0"},
    {"id":"mtk-preloader","label":"MediaTek Preloader","family":"mediatek","repair":true,"priority":80,"product_contains":"preloader"},
    {"id":"qualcomm-edl","label":"Qualcomm EDL","family":"qualcomm","repair":true,"priority":80,"product_contains":"qusb_bulk"},
    {"id":"fastboot","label":"Fastboot","family":"android","repair":true,"priority":50,"if_class":"ff","if_subclass":"42","if_protocol":"03"},
    {"id":"adb","label":"ADB","family":"android","repair":true,"priority":50,"if_class":"ff","if_subclass":"42","if_protocol":"01"},
    {"id":"mtp","label":"MTP","family":"android","priority":20,"if_class":"06","if_subclass":"01","if_protocol":"01"},
    {"id":"mtp","label":"MTP","family":"android","priority":19,"if_class":"ff","if_subclass":"ff","if_protocol":"00","product_contains":"mtp"},
    {"id":"cdc-acm","label":"Serial (CDC ACM)","family":"generic","priority":10,"if_class":"02","if_subclass":"02"},
    {"id":"rndis","label":"RNDIS / USB tethering","family":"android","priority":10,"if_class":"e0","if_subclass":"01","if_protocol":"03"},
    {"id":"mass-storage","label":"Mass Storage","family":"generic","priority":5,"if_class":"08"},
    {"id":"hid","label":"HID","family":"generic","priority":5,"if_class":"03"},
    {"id":"hub","label":"USB Hub","family":"generic","priority":5,"dev_class":"09"}
  ]
})json";

std::string_view ModeDetector::builtin_json() { return kBuiltinSignatures; }

ModeDetector::ModeDetector() { (void)load_json(kBuiltinSignatures, true); }

static int hex_field(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return -1;
    if (it->is_number_integer()) return it->get<int>();
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>(), nullptr, 16);
        } catch (...) {
            return -2;  // invalid
        }
    }
    return -2;
}

Status ModeDetector::load_json(std::string_view text, bool replace) {
    nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("signatures") || !doc["signatures"].is_array())
        return {ErrorCode::MalformedPayload, "mode signature JSON: missing 'signatures' array"};
    std::vector<ModeSignature> loaded;
    for (const auto& j : doc["signatures"]) {
        if (!j.is_object() || !j.contains("id")) return {ErrorCode::MalformedPayload, "signature without id"};
        ModeSignature s;
        s.id = j.value("id", "");
        s.label = j.value("label", s.id);
        s.family = j.value("family", "");
        s.repair = j.value("repair", false);
        s.priority = j.value("priority", 0);
        s.vid = hex_field(j, "vid");
        s.pid = hex_field(j, "pid");
        s.if_class = hex_field(j, "if_class");
        s.if_subclass = hex_field(j, "if_subclass");
        s.if_protocol = hex_field(j, "if_protocol");
        s.dev_class = hex_field(j, "dev_class");
        for (int v : {s.vid, s.pid, s.if_class, s.if_subclass, s.if_protocol, s.dev_class}) {
            if (v == -2) return {ErrorCode::MalformedPayload, "invalid hex value in signature " + s.id};
        }
        s.product_contains = j.value("product_contains", "");
        std::transform(s.product_contains.begin(), s.product_contains.end(), s.product_contains.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        loaded.push_back(std::move(s));
    }
    if (replace) sigs_.clear();
    sigs_.insert(sigs_.end(), loaded.begin(), loaded.end());
    std::stable_sort(sigs_.begin(), sigs_.end(),
                     [](const ModeSignature& a, const ModeSignature& b) { return a.priority > b.priority; });
    return {};
}

DetectedMode ModeDetector::detect(const DescriptorSet& set) const {
    DetectedMode result;
    auto dd = parse_device_descriptor(set.device);
    if (!dd) return result;
    std::vector<InterfaceInfo> ifaces;
    for (const auto& raw : set.configs) {
        if (auto c = parse_configuration(raw)) {
            ifaces.insert(ifaces.end(), c->interfaces.begin(), c->interfaces.end());
        }
    }
    std::string product = find_string(set, dd->iProduct);
    std::transform(product.begin(), product.end(), product.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& s : sigs_) {
        if (s.vid >= 0 && s.vid != dd->idVendor) continue;
        if (s.pid >= 0 && s.pid != dd->idProduct) continue;
        if (s.dev_class >= 0 && s.dev_class != dd->bDeviceClass) continue;
        if (!s.product_contains.empty() && product.find(s.product_contains) == std::string::npos) continue;
        if (s.if_class >= 0 || s.if_subclass >= 0 || s.if_protocol >= 0) {
            bool any = std::any_of(ifaces.begin(), ifaces.end(), [&](const InterfaceInfo& i) {
                return (s.if_class < 0 || s.if_class == i.cls) && (s.if_subclass < 0 || s.if_subclass == i.subclass) &&
                       (s.if_protocol < 0 || s.if_protocol == i.protocol);
            });
            if (!any) continue;
        }
        result.id = s.id;
        result.label = s.label;
        result.family = s.family;
        result.repair_mode = s.repair;
        return result;
    }
    return result;
}

}  // namespace pf
