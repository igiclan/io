#include "Protocol/Protocol.hpp"

#include <cstring>
#include <nlohmann/json.hpp>

namespace pf::proto {

std::string_view to_string(MsgType t) noexcept {
    switch (t) {
        case MsgType::Hello: return "HELLO";
        case MsgType::HelloAck: return "HELLO_ACK";
        case MsgType::Auth: return "AUTH";
        case MsgType::AuthResult: return "AUTH_RESULT";
        case MsgType::SessionResume: return "SESSION_RESUME";
        case MsgType::SessionResumeResult: return "SESSION_RESUME_RESULT";
        case MsgType::DeviceListRequest: return "DEVICE_LIST_REQUEST";
        case MsgType::DeviceList: return "DEVICE_LIST";
        case MsgType::DeviceEvent: return "DEVICE_EVENT";
        case MsgType::DeviceAttach: return "DEVICE_ATTACH";
        case MsgType::DeviceAttachResult: return "DEVICE_ATTACH_RESULT";
        case MsgType::DeviceDetach: return "DEVICE_DETACH";
        case MsgType::DeviceDetachResult: return "DEVICE_DETACH_RESULT";
        case MsgType::DeviceReenumerated: return "DEVICE_REENUMERATED";
        case MsgType::UsbSubmit: return "USB_SUBMIT";
        case MsgType::UsbComplete: return "USB_COMPLETE";
        case MsgType::UsbCancel: return "USB_CANCEL";
        case MsgType::UsbReset: return "USB_RESET";
        case MsgType::UsbOpResult: return "USB_OP_RESULT";
        case MsgType::UsbConfig: return "USB_CONFIG";
        case MsgType::Ping: return "PING";
        case MsgType::Pong: return "PONG";
        case MsgType::Goodbye: return "GOODBYE";
        case MsgType::Error: return "ERROR";
    }
    return "UNKNOWN";
}

bool is_known_type(std::uint8_t t) noexcept { return to_string(static_cast<MsgType>(t)) != "UNKNOWN"; }

bool is_data_plane(MsgType t) noexcept {
    const auto v = static_cast<std::uint8_t>(t);
    return v >= 0x30 && v <= 0x3F;
}

void encode_header(std::uint8_t* out, const FrameHeader& h) noexcept {
    store_le16(out + 0, kMagic);
    out[2] = h.version;
    out[3] = static_cast<std::uint8_t>(h.type);
    store_le16(out + 4, h.flags);
    store_le16(out + 6, static_cast<std::uint16_t>(kHeaderSize));
    store_le32(out + 8, h.payload_size);
    store_le32(out + 12, h.correlation);
}

Result<FrameHeader> decode_header(const std::uint8_t* in, std::uint32_t max_payload) noexcept {
    if (load_le16(in) != kMagic) return Error{ErrorCode::BadMagic, "bad frame magic"};
    FrameHeader h;
    h.version = in[2];
    if (h.version < kMinProtocolVersion || h.version > kProtocolVersion)
        return Error{ErrorCode::UnsupportedVersion, "unsupported protocol version"};
    if (!is_known_type(in[3])) return Error{ErrorCode::ProtocolViolation, "unknown message type"};
    h.type = static_cast<MsgType>(in[3]);
    h.flags = load_le16(in + 4);
    if (load_le16(in + 6) != kHeaderSize) return Error{ErrorCode::ProtocolViolation, "bad header size"};
    h.payload_size = load_le32(in + 8);
    if (h.payload_size > max_payload) return Error{ErrorCode::FrameTooLarge, "frame exceeds max payload"};
    h.correlation = load_le32(in + 12);
    return h;
}

namespace {

OutFrame make_frame(MsgType type, std::size_t payload_size, std::uint16_t flags, std::uint32_t correlation,
                    BufferPool& pool) {
    OutFrame f;
    f.type = type;
    f.buf = pool.acquire(kHeaderSize + payload_size);
    FrameHeader h;
    h.type = type;
    h.flags = flags;
    h.payload_size = static_cast<std::uint32_t>(payload_size);
    h.correlation = correlation;
    encode_header(f.buf.data(), h);
    return f;
}

SpanWriter payload_writer(OutFrame& f) { return SpanWriter(f.buf.span().subspan(kHeaderSize)); }

}  // namespace

OutFrame encode_submit(const UsbSubmitMsg& m, BufferPool& pool) {
    const std::size_t data = m.is_in() ? 0 : m.out_data.size();
    const std::size_t size = kSubmitFixedSize + m.iso.size() * 8 + data;
    OutFrame f = make_frame(MsgType::UsbSubmit, size, 0, 0, pool);
    SpanWriter w = payload_writer(f);
    w.u64(m.seq);
    w.u32(m.device);
    w.u8(m.endpoint);
    w.u8(static_cast<std::uint8_t>(m.type));
    w.u16(m.flags);
    w.u32(m.timeout_ms);
    w.u32(m.is_in() ? m.length : static_cast<std::uint32_t>(data));
    w.bytes(m.setup);
    w.u32(static_cast<std::uint32_t>(m.iso.size()));
    w.u32(0);
    for (const auto& p : m.iso) {
        w.u32(p.offset);
        w.u32(p.length);
    }
    if (data) w.bytes(m.out_data);
    return f;
}

Status decode_submit(ByteSpan payload, UsbSubmitMsg& m) {
    ByteReader r(payload);
    m.seq = r.u64();
    m.device = r.u32();
    m.endpoint = r.u8();
    const std::uint8_t type = r.u8();
    m.flags = r.u16();
    m.timeout_ms = r.u32();
    m.length = r.u32();
    auto setup = r.bytes(8);
    const std::uint32_t iso_count = r.u32();
    r.skip(4);
    if (!r.ok()) return {ErrorCode::MalformedPayload, "USB_SUBMIT truncated"};
    if (type > 3) return {ErrorCode::MalformedPayload, "USB_SUBMIT bad transfer type"};
    m.type = static_cast<TransferType>(type);
    std::memcpy(m.setup.data(), setup.data(), 8);
    if (iso_count > 0 && m.type != TransferType::Isochronous)
        return {ErrorCode::MalformedPayload, "iso packets on non-iso transfer"};
    if (iso_count > r.remaining() / 8) return {ErrorCode::MalformedPayload, "USB_SUBMIT iso count"};
    m.iso.resize(iso_count);
    for (auto& p : m.iso) {
        p.offset = r.u32();
        p.length = r.u32();
        if (static_cast<std::uint64_t>(p.offset) + p.length > m.length)
            return {ErrorCode::MalformedPayload, "iso packet outside buffer"};
    }
    // Direction: for control transfers bmRequestType bit 7 is authoritative.
    const bool in = m.type == TransferType::Control ? (m.setup[0] & 0x80) != 0 : endpoint_is_in(m.endpoint);
    if (in != m.is_in()) return {ErrorCode::MalformedPayload, "USB_SUBMIT direction mismatch"};
    if (in) {
        if (r.remaining() != 0) return {ErrorCode::MalformedPayload, "data on IN submit"};
        m.out_data = {};
    } else {
        if (r.remaining() != m.length) return {ErrorCode::MalformedPayload, "USB_SUBMIT OUT length mismatch"};
        m.out_data = r.bytes(m.length);
    }
    return {};
}

OutFrame encode_complete(const UsbCompleteMsg& m, BufferPool& pool) {
    const std::size_t size = kCompleteFixedSize + m.iso.size() * 8 + m.in_data.size();
    OutFrame f = make_frame(MsgType::UsbComplete, size, frame_flags::kResponse, 0, pool);
    SpanWriter w = payload_writer(f);
    w.u64(m.seq);
    w.u32(m.device);
    w.i32(static_cast<std::int32_t>(m.status));
    w.u32(m.actual_length);
    w.u32(static_cast<std::uint32_t>(m.iso.size()));
    w.u32(m.iso_error_count);
    w.u32(0);
    for (const auto& r : m.iso) {
        w.u32(r.actual_length);
        w.i32(r.status);
    }
    w.bytes(m.in_data);
    return f;
}

OutFrame finalize_complete_inplace(PooledBuffer&& buf, const UsbCompleteMsg& m, std::size_t data_len) {
    const std::size_t prefix = complete_prefix_size(m.iso.size());
    OutFrame f;
    f.type = MsgType::UsbComplete;
    f.buf = std::move(buf);
    f.buf.resize(prefix + data_len);
    FrameHeader h;
    h.type = MsgType::UsbComplete;
    h.flags = frame_flags::kResponse;
    h.payload_size = static_cast<std::uint32_t>(prefix - kHeaderSize + data_len);
    encode_header(f.buf.data(), h);
    SpanWriter w(f.buf.span().subspan(kHeaderSize, prefix - kHeaderSize));
    w.u64(m.seq);
    w.u32(m.device);
    w.i32(static_cast<std::int32_t>(m.status));
    w.u32(m.actual_length);
    w.u32(static_cast<std::uint32_t>(m.iso.size()));
    w.u32(m.iso_error_count);
    w.u32(0);
    for (const auto& r : m.iso) {
        w.u32(r.actual_length);
        w.i32(r.status);
    }
    return f;
}

Status decode_complete(ByteSpan payload, UsbCompleteMsg& m) {
    ByteReader r(payload);
    m.seq = r.u64();
    m.device = r.u32();
    m.status = static_cast<UsbStatus>(r.i32());
    m.actual_length = r.u32();
    const std::uint32_t iso_count = r.u32();
    m.iso_error_count = r.u32();
    r.skip(4);
    if (!r.ok()) return {ErrorCode::MalformedPayload, "USB_COMPLETE truncated"};
    if (iso_count > r.remaining() / 8) return {ErrorCode::MalformedPayload, "USB_COMPLETE iso count"};
    m.iso.resize(iso_count);
    for (auto& i : m.iso) {
        i.actual_length = r.u32();
        i.status = r.i32();
    }
    m.in_data = r.bytes(r.remaining());
    if (iso_count == 0 && !m.in_data.empty() && m.in_data.size() != m.actual_length)
        return {ErrorCode::MalformedPayload, "USB_COMPLETE data length mismatch"};
    return {};
}

OutFrame encode_cancel(const UsbCancelMsg& m, BufferPool& pool) {
    OutFrame f = make_frame(MsgType::UsbCancel, 12, 0, 0, pool);
    SpanWriter w = payload_writer(f);
    w.u64(m.seq);
    w.u32(m.device);
    return f;
}

Status decode_cancel(ByteSpan payload, UsbCancelMsg& m) {
    ByteReader r(payload);
    m.seq = r.u64();
    m.device = r.u32();
    if (!r.ok()) return {ErrorCode::MalformedPayload, "USB_CANCEL truncated"};
    return {};
}

OutFrame encode_reset(const UsbResetMsg& m, BufferPool& pool) {
    OutFrame f = make_frame(MsgType::UsbReset, 16, 0, 0, pool);
    SpanWriter w = payload_writer(f);
    w.u64(m.seq);
    w.u32(m.device);
    w.u8(static_cast<std::uint8_t>(m.kind));
    w.zeros(3);
    return f;
}

Status decode_reset(ByteSpan payload, UsbResetMsg& m) {
    ByteReader r(payload);
    m.seq = r.u64();
    m.device = r.u32();
    const std::uint8_t kind = r.u8();
    if (!r.ok() || kind > 1) return {ErrorCode::MalformedPayload, "USB_RESET"};
    m.kind = static_cast<ResetKind>(kind);
    return {};
}

OutFrame encode_config(const UsbConfigMsg& m, BufferPool& pool) {
    OutFrame f = make_frame(MsgType::UsbConfig, 16, 0, 0, pool);
    SpanWriter w = payload_writer(f);
    w.u64(m.seq);
    w.u32(m.device);
    w.u8(static_cast<std::uint8_t>(m.op));
    w.u8(m.value);
    w.u8(m.alt);
    w.u8(0);
    return f;
}

Status decode_config(ByteSpan payload, UsbConfigMsg& m) {
    ByteReader r(payload);
    m.seq = r.u64();
    m.device = r.u32();
    const std::uint8_t op = r.u8();
    m.value = r.u8();
    m.alt = r.u8();
    if (!r.ok() || op > 2) return {ErrorCode::MalformedPayload, "USB_CONFIG"};
    m.op = static_cast<ConfigOp>(op);
    return {};
}

OutFrame encode_op_result(const UsbOpResultMsg& m, BufferPool& pool) {
    OutFrame f = make_frame(MsgType::UsbOpResult, 16, frame_flags::kResponse, 0, pool);
    SpanWriter w = payload_writer(f);
    w.u64(m.seq);
    w.u32(m.device);
    w.i32(static_cast<std::int32_t>(m.status));
    return f;
}

Status decode_op_result(ByteSpan payload, UsbOpResultMsg& m) {
    ByteReader r(payload);
    m.seq = r.u64();
    m.device = r.u32();
    m.status = static_cast<UsbStatus>(r.i32());
    if (!r.ok()) return {ErrorCode::MalformedPayload, "USB_OP_RESULT"};
    return {};
}

OutFrame encode_ping(MsgType type, const PingMsg& m, BufferPool& pool) {
    OutFrame f = make_frame(type, 16, type == MsgType::Pong ? frame_flags::kResponse : 0, 0, pool);
    SpanWriter w = payload_writer(f);
    w.u64(m.seq);
    w.u64(m.sent_ns);
    return f;
}

Status decode_ping(ByteSpan payload, PingMsg& m) {
    ByteReader r(payload);
    m.seq = r.u64();
    m.sent_ns = r.u64();
    if (!r.ok()) return {ErrorCode::MalformedPayload, "PING"};
    return {};
}

OutFrame encode_json(MsgType type, const nlohmann::json& body, std::uint32_t correlation, bool response,
                     BufferPool& pool) {
    const std::string text = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::uint16_t flags = frame_flags::kJson;
    if (response) flags |= frame_flags::kResponse;
    OutFrame f = make_frame(type, text.size(), flags, correlation, pool);
    if (!text.empty()) std::memcpy(f.buf.data() + kHeaderSize, text.data(), text.size());
    return f;
}

OutFrame encode_error(ErrorCode code, std::string_view message, std::uint32_t correlation, BufferPool& pool) {
    nlohmann::json j{{"code", static_cast<int>(code)}, {"name", std::string(to_string(code))},
                     {"message", std::string(message)}};
    return encode_json(MsgType::Error, j, correlation, correlation != 0, pool);
}

Result<nlohmann::json> decode_json(const InFrame& f) {
    if ((f.header.flags & frame_flags::kJson) == 0) return Error{ErrorCode::MalformedPayload, "expected JSON payload"};
    auto bytes = f.bytes();
    auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (j.is_discarded()) return Error{ErrorCode::MalformedPayload, "invalid JSON"};
    if (!j.is_object()) return Error{ErrorCode::MalformedPayload, "JSON payload must be an object"};
    return j;
}

// ------------------------------------------------------------- FrameDecoder

FrameDecoder::FrameDecoder(std::uint32_t max_payload, std::size_t staging_size, BufferPool& pool)
    : max_payload_(max_payload), pool_(pool), staging_(staging_size < 4096 ? 4096 : staging_size) {}

MutableByteSpan FrameDecoder::prepare() {
    if (direct_) {
        return MutableByteSpan(pending_.payload.data() + pending_filled_, pending_.payload.size() - pending_filled_);
    }
    if (staging_end_ == staging_.size()) {
        // compact
        const std::size_t live = staging_end_ - staging_begin_;
        std::memmove(staging_.data(), staging_.data() + staging_begin_, live);
        staging_begin_ = 0;
        staging_end_ = live;
    }
    return MutableByteSpan(staging_.data() + staging_end_, staging_.size() - staging_end_);
}

Status FrameDecoder::commit(std::size_t n, const FrameHandler& on_frame) {
    if (direct_) {
        pending_filled_ += n;
        if (pending_filled_ == pending_.payload.size()) {
            direct_ = false;
            InFrame f = std::move(pending_);
            pending_ = InFrame{};
            pending_filled_ = 0;
            on_frame(std::move(f));
        }
        return {};
    }
    staging_end_ += n;
    return parse_staging(on_frame);
}

Status FrameDecoder::parse_staging(const FrameHandler& on_frame) {
    // Payloads larger than this are completed by reading straight into the frame buffer.
    const std::size_t direct_threshold = staging_.size() / 2;
    while (staging_end_ - staging_begin_ >= kHeaderSize) {
        const std::uint8_t* p = staging_.data() + staging_begin_;
        auto hdr = decode_header(p, max_payload_);
        if (!hdr) return hdr.error();
        const std::size_t avail = staging_end_ - staging_begin_ - kHeaderSize;
        const std::size_t need = hdr->payload_size;
        if (avail >= need) {
            InFrame f;
            f.header = *hdr;
            f.payload = pool_.acquire(need);
            f.payload.resize(need);
            if (need) std::memcpy(f.payload.data(), p + kHeaderSize, need);
            staging_begin_ += kHeaderSize + need;
            on_frame(std::move(f));
            continue;
        }
        if (need > direct_threshold) {
            pending_.header = *hdr;
            pending_.payload = pool_.acquire(need);
            pending_.payload.resize(need);
            if (avail) std::memcpy(pending_.payload.data(), p + kHeaderSize, avail);
            pending_filled_ = avail;
            direct_ = true;
            staging_begin_ = staging_end_ = 0;
            return {};
        }
        break;  // wait for more bytes in staging
    }
    if (staging_begin_ == staging_end_) {
        staging_begin_ = staging_end_ = 0;
    } else if (staging_begin_ > staging_.size() / 2) {
        const std::size_t live = staging_end_ - staging_begin_;
        std::memmove(staging_.data(), staging_.data() + staging_begin_, live);
        staging_begin_ = 0;
        staging_end_ = live;
    }
    return {};
}

}  // namespace pf::proto
