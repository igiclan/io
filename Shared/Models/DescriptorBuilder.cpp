#include "Models/DescriptorBuilder.hpp"

namespace pf {

DescriptorSet build_descriptor_set(const DeviceSpec& spec) {
    DescriptorSet set;
    std::uint8_t next_string = 1;
    auto add_string = [&](const std::string& s) -> std::uint8_t {
        if (s.empty()) return 0;
        StringDescriptor sd;
        sd.index = next_string++;
        sd.lang_id = 0x0409;
        sd.raw = make_string_descriptor(s);
        set.strings.push_back(std::move(sd));
        return set.strings.back().index;
    };
    set.strings.push_back(StringDescriptor{0, 0, Bytes{4, usb_desc::kString, 0x09, 0x04}});
    const std::uint8_t i_manufacturer = add_string(spec.manufacturer);
    const std::uint8_t i_product = add_string(spec.product);
    const std::uint8_t i_serial = add_string(spec.serial);

    set.device = Bytes(18);
    auto& d = set.device;
    d[0] = 18;
    d[1] = usb_desc::kDevice;
    store_le16(&d[2], spec.bcd_usb);
    d[4] = spec.dev_class;
    d[5] = spec.dev_subclass;
    d[6] = spec.dev_protocol;
    d[7] = spec.max_packet0;
    store_le16(&d[8], spec.vid);
    store_le16(&d[10], spec.pid);
    store_le16(&d[12], spec.bcd_device);
    d[14] = i_manufacturer;
    d[15] = i_product;
    d[16] = i_serial;
    d[17] = 1;

    Bytes cfg{9, usb_desc::kConfiguration, 0, 0, 0, 1, 0, 0x80, 50};
    std::uint8_t num_interfaces = 0;
    for (const auto& itf : spec.interfaces) {
        if (itf.alt == 0) ++num_interfaces;
        const std::uint8_t i_itf = add_string(itf.name);
        cfg.insert(cfg.end(), {9, usb_desc::kInterface, itf.number, itf.alt,
                               static_cast<std::uint8_t>(itf.endpoints.size()), itf.cls, itf.subclass, itf.protocol,
                               i_itf});
        for (const auto& ep : itf.endpoints) {
            cfg.insert(cfg.end(), {7, usb_desc::kEndpoint, ep.address, static_cast<std::uint8_t>(ep.type),
                                   static_cast<std::uint8_t>(ep.max_packet & 0xFF),
                                   static_cast<std::uint8_t>(ep.max_packet >> 8), ep.interval});
        }
    }
    cfg[4] = num_interfaces;
    store_le16(&cfg[2], static_cast<std::uint16_t>(cfg.size()));
    set.configs.push_back(std::move(cfg));
    return set;
}

}  // namespace pf
