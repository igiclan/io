#include "VirtualUSB/VhciRecords.hpp"

#include <cstring>

#include "../../Shared/Driver/pfvhci_ioctl.h"

namespace pf::client {

namespace {

constexpr std::size_t kHdr = sizeof(PFVHCI_RECORD_HEADER);

void pad8(Bytes& b) {
    while (b.size() % 8) b.push_back(0);
}

void write_header(SpanWriter& w, std::uint32_t size, std::uint16_t type, std::uint32_t handle, std::uint64_t cookie,
                  std::uint64_t request_id) {
    w.u32(size);
    w.u16(type);
    w.u16(0);
    w.u32(handle);
    w.u32(0);
    w.u64(cookie);
    w.u64(request_id);
}

struct Header {
    std::uint32_t size;
    std::uint16_t type;
    std::uint32_t handle;
    std::uint64_t cookie;
    std::uint64_t request_id;
};

Header read_header(ByteReader& r) {
    Header h{};
    h.size = r.u32();
    h.type = r.u16();
    r.u16();
    h.handle = r.u32();
    r.u32();
    h.cookie = r.u64();
    h.request_id = r.u64();
    return h;
}

}  // namespace

// ------------------------------------------------------------------ plugin

Bytes encode_plugin(const DescriptorSet& set, UsbSpeed speed, std::uint64_t cookie) {
    struct Entry {
        std::uint8_t type, index;
        std::uint16_t lang;
        const Bytes* data;
    };
    std::vector<Entry> entries;
    entries.push_back({0x01, 0, 0, &set.device});
    for (std::size_t i = 0; i < set.configs.size(); ++i)
        entries.push_back({0x02, static_cast<std::uint8_t>(i), 0, &set.configs[i]});
    if (!set.bos.empty()) entries.push_back({0x0F, 0, 0, &set.bos});
    if (!set.device_qualifier.empty()) entries.push_back({0x06, 0, 0, &set.device_qualifier});
    for (const auto& s : set.strings) entries.push_back({0x03, s.index, s.lang_id, &s.raw});

    Bytes out(sizeof(PFVHCI_PLUGIN_HEADER), 0);
    for (const auto& e : entries) {
        const std::size_t at = out.size();
        out.resize(at + sizeof(PFVHCI_DESCRIPTOR_ENTRY));
        SpanWriter w(MutableByteSpan(out.data() + at, sizeof(PFVHCI_DESCRIPTOR_ENTRY)));
        w.u8(e.type);
        w.u8(e.index);
        w.u16(e.lang);
        w.u32(static_cast<std::uint32_t>(e.data->size()));
        out.insert(out.end(), e.data->begin(), e.data->end());
        pad8(out);
    }
    SpanWriter w(MutableByteSpan(out.data(), sizeof(PFVHCI_PLUGIN_HEADER)));
    w.u32(static_cast<std::uint32_t>(out.size()));
    w.u32(PFVHCI_ABI_VERSION);
    w.u64(cookie);
    w.u8(static_cast<std::uint8_t>(speed));
    w.zeros(3);
    w.u32(static_cast<std::uint32_t>(entries.size()));
    return out;
}

Result<DescriptorSet> decode_plugin(ByteSpan buf, UsbSpeed& speed, std::uint64_t& cookie) {
    ByteReader r(buf);
    const std::uint32_t size = r.u32();
    const std::uint32_t abi = r.u32();
    cookie = r.u64();
    const std::uint8_t sp = r.u8();
    r.skip(3);
    const std::uint32_t count = r.u32();
    if (!r.ok() || size != buf.size()) return Error{ErrorCode::MalformedPayload, "plugin size"};
    if (abi != PFVHCI_ABI_VERSION) return Error{ErrorCode::DriverVersionMismatch, "plugin ABI"};
    if (sp < PFVHCI_SPEED_LOW || sp > PFVHCI_SPEED_SUPER_PLUS) return Error{ErrorCode::MalformedPayload, "speed"};
    speed = static_cast<UsbSpeed>(sp);
    DescriptorSet set;
    std::size_t total = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t type = r.u8();
        const std::uint8_t index = r.u8();
        const std::uint16_t lang = r.u16();
        const std::uint32_t len = r.u32();
        if (!r.ok() || len > r.remaining() || len > PFVHCI_MAX_DESCRIPTOR_BYTES)
            return Error{ErrorCode::MalformedPayload, "plugin entry"};
        auto data = r.bytes(len);
        total += len;
        if (total > PFVHCI_MAX_DESCRIPTOR_BYTES) return Error{ErrorCode::MalformedPayload, "descriptors too large"};
        r.skip(PFVHCI_ALIGN8(len) - len);
        Bytes b(data.begin(), data.end());
        switch (type) {
            case 0x01: set.device = std::move(b); break;
            case 0x02: set.configs.push_back(std::move(b)); break;
            case 0x0F: set.bos = std::move(b); break;
            case 0x06: set.device_qualifier = std::move(b); break;
            case 0x03: set.strings.push_back(StringDescriptor{index, lang, std::move(b)}); break;
            default: return Error{ErrorCode::MalformedPayload, "unknown descriptor entry type"};
        }
    }
    if (!r.ok() || r.remaining() != 0) return Error{ErrorCode::MalformedPayload, "plugin trailing bytes"};
    std::string why;
    if (!validate_descriptor_set(set, &why)) return Error{ErrorCode::MalformedPayload, why};
    return set;
}

// ------------------------------------------------------------------ fetch

Result<std::vector<VhciRequest>> parse_fetch_buffer(ByteSpan buf) {
    std::vector<VhciRequest> out;
    std::size_t pos = 0;
    while (pos < buf.size()) {
        if (buf.size() - pos < kHdr) return Error{ErrorCode::DriverError, "truncated record header"};
        ByteReader r(buf.subspan(pos));
        const Header h = read_header(r);
        if (h.size < kHdr || h.size % 8 || h.size > buf.size() - pos)
            return Error{ErrorCode::DriverError, "bad record size"};
        ByteReader body(buf.subspan(pos + kHdr, h.size - kHdr));
        VhciRequest q;
        q.device_handle = h.handle;
        q.cookie = h.cookie;
        q.request_id = h.request_id;
        q.kind = static_cast<VhciRecordKind>(h.type);
        switch (h.type) {
            case PFVHCI_REC_SUBMIT: {
                q.endpoint = body.u8();
                const std::uint8_t t = body.u8();
                q.flags = body.u16();
                q.length = body.u32();
                q.timeout_ms = body.u32();
                const std::uint32_t iso = body.u32();
                auto setup = body.bytes(8);
                if (!body.ok() || t > 3 || iso > PFVHCI_MAX_ISO_PACKETS || q.length > PFVHCI_MAX_TRANSFER_BYTES)
                    return Error{ErrorCode::DriverError, "bad SUBMIT record"};
                q.type = static_cast<TransferType>(t);
                std::memcpy(q.setup.data(), setup.data(), 8);
                q.iso.resize(iso);
                for (auto& p : q.iso) {
                    p.offset = body.u32();
                    p.length = body.u32();
                }
                if (!q.is_in()) {
                    q.out_data = body.bytes(q.length);
                }
                if (!body.ok()) return Error{ErrorCode::DriverError, "truncated SUBMIT record"};
                break;
            }
            case PFVHCI_REC_CANCEL:
            case PFVHCI_REC_ENDPOINT_RESET:
            case PFVHCI_REC_SELECT_CONFIG:
            case PFVHCI_REC_SELECT_INTERFACE:
            case PFVHCI_REC_DEVICE_RESET:
            case PFVHCI_REC_DEVICE_REMOVED:
                q.op_endpoint = body.u8();
                q.config_value = body.u8();
                q.interface_number = body.u8();
                q.alt_setting = body.u8();
                if (!body.ok()) return Error{ErrorCode::DriverError, "truncated op record"};
                break;
            default:
                return Error{ErrorCode::DriverError, "unknown record type " + std::to_string(h.type)};
        }
        out.push_back(std::move(q));
        pos += h.size;
    }
    return out;
}

void append_completion(Bytes& out, const VhciCompletion& c) {
    const std::size_t at = out.size();
    if (c.is_op_result) {
        const std::uint32_t size = sizeof(PFVHCI_OP_RESULT);
        out.resize(at + size);
        SpanWriter w(MutableByteSpan(out.data() + at, size));
        write_header(w, size, PFVHCI_REC_OP_RESULT, c.device_handle, c.cookie, c.request_id);
        w.i32(static_cast<std::int32_t>(c.status));
        w.u32(0);
        return;
    }
    const std::size_t raw = sizeof(PFVHCI_COMPLETION) + c.iso.size() * sizeof(PFVHCI_ISO_RESULT) + c.in_data.size();
    const std::uint32_t size = static_cast<std::uint32_t>(PFVHCI_ALIGN8(raw));
    out.resize(at + size, 0);
    SpanWriter w(MutableByteSpan(out.data() + at, size));
    write_header(w, size, PFVHCI_REC_COMPLETION, c.device_handle, c.cookie, c.request_id);
    w.i32(static_cast<std::int32_t>(c.status));
    w.u32(c.actual_length);
    w.u32(static_cast<std::uint32_t>(c.iso.size()));
    w.u32(c.iso_error_count);
    for (const auto& i : c.iso) {
        w.u32(i.actual_length);
        w.i32(i.status);
    }
    w.bytes(c.in_data);
}

// ------------------------------------------------------------------ driver side (test double)

void append_request(Bytes& out, const VhciRequest& q) {
    const std::size_t at = out.size();
    if (q.kind == VhciRecordKind::Submit) {
        const std::size_t data = q.is_in() ? 0 : q.out_data.size();
        const std::size_t raw = sizeof(PFVHCI_SUBMIT) + q.iso.size() * sizeof(PFVHCI_ISO_PACKET) + data;
        const std::uint32_t size = static_cast<std::uint32_t>(PFVHCI_ALIGN8(raw));
        out.resize(at + size, 0);
        SpanWriter w(MutableByteSpan(out.data() + at, size));
        write_header(w, size, PFVHCI_REC_SUBMIT, q.device_handle, q.cookie, q.request_id);
        w.u8(q.endpoint);
        w.u8(static_cast<std::uint8_t>(q.type));
        w.u16(q.flags);
        w.u32(q.is_in() ? q.length : static_cast<std::uint32_t>(data));
        w.u32(q.timeout_ms);
        w.u32(static_cast<std::uint32_t>(q.iso.size()));
        w.bytes(q.setup);
        for (const auto& p : q.iso) {
            w.u32(p.offset);
            w.u32(p.length);
        }
        if (data) w.bytes(q.out_data);
        return;
    }
    const std::uint32_t size = sizeof(PFVHCI_ENDPOINT_OP);
    out.resize(at + size, 0);
    SpanWriter w(MutableByteSpan(out.data() + at, size));
    write_header(w, size, static_cast<std::uint16_t>(q.kind), q.device_handle, q.cookie, q.request_id);
    w.u8(q.op_endpoint);
    w.u8(q.config_value);
    w.u8(q.interface_number);
    w.u8(q.alt_setting);
    w.u32(0);
}

Result<std::vector<VhciCompletion>> parse_complete_buffer(ByteSpan buf) {
    std::vector<VhciCompletion> out;
    std::size_t pos = 0;
    while (pos < buf.size()) {
        if (buf.size() - pos < kHdr) return Error{ErrorCode::MalformedPayload, "truncated completion header"};
        ByteReader r(buf.subspan(pos));
        const Header h = read_header(r);
        if (h.size < kHdr || h.size % 8 || h.size > buf.size() - pos)
            return Error{ErrorCode::MalformedPayload, "bad completion size"};
        ByteReader body(buf.subspan(pos + kHdr, h.size - kHdr));
        VhciCompletion c;
        c.device_handle = h.handle;
        c.cookie = h.cookie;
        c.request_id = h.request_id;
        c.status = static_cast<UsbStatus>(body.i32());
        if (h.type == PFVHCI_REC_OP_RESULT) {
            c.is_op_result = true;
        } else if (h.type == PFVHCI_REC_COMPLETION) {
            c.actual_length = body.u32();
            const std::uint32_t iso = body.u32();
            c.iso_error_count = body.u32();
            if (iso > PFVHCI_MAX_ISO_PACKETS) return Error{ErrorCode::MalformedPayload, "iso count"};
            c.iso.resize(iso);
            for (auto& i : c.iso) {
                i.actual_length = body.u32();
                i.status = body.i32();
            }
            // Data follows only for IN transfers; the record does not say which, so
            // hand back the remainder (data + alignment padding) and let the caller,
            // which knows the direction of the original SUBMIT, take actual_length.
            c.in_data = body.bytes(body.remaining());
        } else {
            return Error{ErrorCode::MalformedPayload, "unknown completion type"};
        }
        if (!body.ok()) return Error{ErrorCode::MalformedPayload, "truncated completion"};
        out.push_back(std::move(c));
        pos += h.size;
    }
    return out;
}

}  // namespace pf::client
