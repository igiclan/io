#include "Models/Descriptors.hpp"

#include <cstdio>

namespace pf {

std::optional<DeviceDescriptor> parse_device_descriptor(ByteSpan raw) {
    if (raw.size() < 18 || raw[0] < 18 || raw[1] != usb_desc::kDevice) return std::nullopt;
    DeviceDescriptor d;
    d.bcdUSB = load_le16(&raw[2]);
    d.bDeviceClass = raw[4];
    d.bDeviceSubClass = raw[5];
    d.bDeviceProtocol = raw[6];
    d.bMaxPacketSize0 = raw[7];
    d.idVendor = load_le16(&raw[8]);
    d.idProduct = load_le16(&raw[10]);
    d.bcdDevice = load_le16(&raw[12]);
    d.iManufacturer = raw[14];
    d.iProduct = raw[15];
    d.iSerialNumber = raw[16];
    d.bNumConfigurations = raw[17];
    return d;
}

std::optional<ConfigurationInfo> parse_configuration(ByteSpan raw) {
    if (raw.size() < 9 || raw[0] < 9 || raw[1] != usb_desc::kConfiguration) return std::nullopt;
    const std::uint16_t total = load_le16(&raw[2]);
    if (total < 9 || total > raw.size()) return std::nullopt;
    ConfigurationInfo c;
    c.num_interfaces = raw[4];
    c.value = raw[5];
    c.attributes = raw[7];
    c.max_power_ma = static_cast<std::uint16_t>(raw[8] * 2);

    std::size_t pos = raw[0];
    InterfaceInfo* current = nullptr;
    while (pos + 2 <= total) {
        const std::uint8_t len = raw[pos];
        const std::uint8_t type = raw[pos + 1];
        if (len < 2 || pos + len > total) return std::nullopt;  // malformed
        if (type == usb_desc::kInterface && len >= 9) {
            InterfaceInfo i;
            i.number = raw[pos + 2];
            i.alt_setting = raw[pos + 3];
            i.cls = raw[pos + 5];
            i.subclass = raw[pos + 6];
            i.protocol = raw[pos + 7];
            i.iInterface = raw[pos + 8];
            c.interfaces.push_back(std::move(i));
            current = &c.interfaces.back();
        } else if (type == usb_desc::kEndpoint && len >= 7) {
            if (current == nullptr) return std::nullopt;  // endpoint outside interface
            EndpointInfo e;
            e.address = raw[pos + 2];
            e.attributes = raw[pos + 3];
            e.max_packet = load_le16(&raw[pos + 4]);
            e.interval = raw[pos + 6];
            current->endpoints.push_back(e);
        }
        pos += len;
    }
    return c;
}

std::string decode_string_descriptor(ByteSpan raw) {
    if (raw.size() < 2 || raw[1] != usb_desc::kString) return {};
    std::size_t len = raw[0] < raw.size() ? raw[0] : raw.size();
    std::string out;
    for (std::size_t i = 2; i + 1 < len; i += 2) {
        std::uint32_t cp = load_le16(&raw[i]);
        // surrogate pair
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {
            std::uint32_t lo = load_le16(&raw[i + 2]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (cp == 0) break;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

Bytes make_string_descriptor(std::string_view utf8) {
    Bytes out{0, usb_desc::kString};
    for (std::size_t i = 0; i < utf8.size() && out.size() < 254;) {
        auto c = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp;
        if (c < 0x80) { cp = c; i += 1; }
        else if ((c >> 5) == 0x6 && i + 1 < utf8.size()) { cp = ((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F); i += 2; }
        else if ((c >> 4) == 0xE && i + 2 < utf8.size()) {
            cp = ((c & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F);
            i += 3;
        } else { cp = '?'; i += 1; }
        out.push_back(static_cast<std::uint8_t>(cp));
        out.push_back(static_cast<std::uint8_t>(cp >> 8));
    }
    out[0] = static_cast<std::uint8_t>(out.size());
    return out;
}

std::string find_string(const DescriptorSet& set, std::uint8_t index) {
    if (index == 0) return {};
    for (const auto& s : set.strings) {
        if (s.index == index) return decode_string_descriptor(s.raw);
    }
    return {};
}

bool validate_descriptor_set(const DescriptorSet& set, std::string* why) {
    auto fail = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    auto dd = parse_device_descriptor(set.device);
    if (!dd) return fail("invalid device descriptor");
    if (dd->bMaxPacketSize0 != 8 && dd->bMaxPacketSize0 != 16 && dd->bMaxPacketSize0 != 32 &&
        dd->bMaxPacketSize0 != 64 && dd->bMaxPacketSize0 != 9 /* SuperSpeed: 2^9 */)
        return fail("invalid bMaxPacketSize0");
    if (set.configs.empty()) return fail("no configuration descriptors");
    for (const auto& c : set.configs) {
        if (!parse_configuration(c)) return fail("invalid configuration descriptor");
    }
    if (!set.bos.empty()) {
        if (set.bos.size() < 5 || set.bos[1] != usb_desc::kBos || load_le16(&set.bos[2]) != set.bos.size())
            return fail("invalid BOS descriptor");
    }
    for (const auto& s : set.strings) {
        if (s.raw.size() < 2 || s.raw[1] != usb_desc::kString || s.raw[0] != s.raw.size())
            return fail("invalid string descriptor");
    }
    return true;
}

std::string format_bcd(std::uint16_t bcd) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%x.%02x", bcd >> 8, bcd & 0xFF);
    return buf;
}

}  // namespace pf
