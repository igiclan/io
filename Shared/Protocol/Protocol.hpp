// PFUP/1 - ParsFlash USB Protocol, version 1. Normative spec: Docs/PROTOCOL.md.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "Common/BufferPool.hpp"
#include "Common/ByteIo.hpp"
#include "Common/Error.hpp"
#include "Models/UsbTypes.hpp"

namespace pf::proto {

inline constexpr std::uint16_t kMagic = 0x4650;  // bytes on the wire: 'P' 'F'
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::uint8_t kMinProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::uint32_t kDefaultMaxPayload = 16u * 1024 * 1024 + 64 * 1024;
inline constexpr std::uint16_t kDefaultPort = 39000;

enum class MsgType : std::uint8_t {
    // session / control plane (JSON payload)
    Hello = 0x01,
    HelloAck = 0x02,
    Auth = 0x03,
    AuthResult = 0x04,
    SessionResume = 0x05,
    SessionResumeResult = 0x06,
    DeviceListRequest = 0x10,
    DeviceList = 0x11,
    DeviceEvent = 0x12,
    DeviceAttach = 0x20,
    DeviceAttachResult = 0x21,
    DeviceDetach = 0x22,
    DeviceDetachResult = 0x23,
    DeviceReenumerated = 0x24,
    // data plane (binary payload)
    UsbSubmit = 0x30,     // a.k.a. USB_REQUEST
    UsbComplete = 0x31,   // a.k.a. USB_RESPONSE
    UsbCancel = 0x32,
    UsbReset = 0x33,
    UsbOpResult = 0x34,
    UsbConfig = 0x35,
    // link
    Ping = 0x40,
    Pong = 0x41,
    Goodbye = 0x7E,
    Error = 0x7F,
};

std::string_view to_string(MsgType t) noexcept;
bool is_known_type(std::uint8_t t) noexcept;
bool is_data_plane(MsgType t) noexcept;

namespace frame_flags {
inline constexpr std::uint16_t kJson = 0x0001;      // payload is UTF-8 JSON
inline constexpr std::uint16_t kResponse = 0x0002;  // reply to the request with the same correlation id
}  // namespace frame_flags

struct FrameHeader {
    std::uint8_t version = kProtocolVersion;
    MsgType type = MsgType::Ping;
    std::uint16_t flags = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t correlation = 0;
};

void encode_header(std::uint8_t* out, const FrameHeader& h) noexcept;
// Validates magic, version range, header size, known type and payload bound.
Result<FrameHeader> decode_header(const std::uint8_t* in, std::uint32_t max_payload) noexcept;

// A frame ready to send: header + payload in one pooled buffer.
struct OutFrame {
    PooledBuffer buf;
    MsgType type = MsgType::Ping;
    std::uint8_t priority = 1;  // SessionPriority as integer, used by the send scheduler
};

// A received frame. payload points to the whole payload.
struct InFrame {
    FrameHeader header;
    PooledBuffer payload;
    [[nodiscard]] ByteSpan bytes() const noexcept { return payload.span(); }
};

// ---------------------------------------------------------------- data plane

struct IsoPacket {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};
struct IsoResult {
    std::uint32_t actual_length = 0;
    std::int32_t status = 0;  // UsbStatus
};

struct UsbSubmitMsg {
    std::uint64_t seq = 0;
    std::uint32_t device = 0;  // attach handle
    std::uint8_t endpoint = 0;
    TransferType type = TransferType::Bulk;
    std::uint16_t flags = 0;   // xfer_flags
    std::uint32_t timeout_ms = 0;
    std::uint32_t length = 0;  // IN: requested; OUT: data size
    std::array<std::uint8_t, 8> setup{};
    std::vector<IsoPacket> iso;
    ByteSpan out_data;         // OUT only; points into the frame payload when decoded
    [[nodiscard]] bool is_in() const noexcept {
        return (flags & xfer_flags::kDirIn) != 0;
    }
};
inline constexpr std::size_t kSubmitFixedSize = 40;

struct UsbCompleteMsg {
    std::uint64_t seq = 0;
    std::uint32_t device = 0;
    UsbStatus status = UsbStatus::Success;
    std::uint32_t actual_length = 0;
    std::uint32_t iso_error_count = 0;
    std::vector<IsoResult> iso;
    ByteSpan in_data;  // IN only
};
inline constexpr std::size_t kCompleteFixedSize = 32;

struct UsbCancelMsg {
    std::uint64_t seq = 0;
    std::uint32_t device = 0;
};

enum class ResetKind : std::uint8_t { PortReset = 0, Reenumerate = 1 };
struct UsbResetMsg {
    std::uint64_t seq = 0;
    std::uint32_t device = 0;
    ResetKind kind = ResetKind::PortReset;
};

enum class ConfigOp : std::uint8_t { SetConfiguration = 0, SetInterface = 1, ClearHalt = 2 };
struct UsbConfigMsg {
    std::uint64_t seq = 0;
    std::uint32_t device = 0;
    ConfigOp op = ConfigOp::SetConfiguration;
    std::uint8_t value = 0;   // configuration value | interface number | endpoint address
    std::uint8_t alt = 0;     // alternate setting (SetInterface)
};

struct UsbOpResultMsg {
    std::uint64_t seq = 0;
    std::uint32_t device = 0;
    UsbStatus status = UsbStatus::Success;
};

struct PingMsg {
    std::uint64_t seq = 0;
    std::uint64_t sent_ns = 0;  // sender's monotonic clock, echoed verbatim
};

// Encoders return a complete frame (header + payload).
OutFrame encode_submit(const UsbSubmitMsg& m, BufferPool& pool = BufferPool::global());
OutFrame encode_complete(const UsbCompleteMsg& m, BufferPool& pool = BufferPool::global());
OutFrame encode_cancel(const UsbCancelMsg& m, BufferPool& pool = BufferPool::global());
OutFrame encode_reset(const UsbResetMsg& m, BufferPool& pool = BufferPool::global());
OutFrame encode_config(const UsbConfigMsg& m, BufferPool& pool = BufferPool::global());
OutFrame encode_op_result(const UsbOpResultMsg& m, BufferPool& pool = BufferPool::global());
OutFrame encode_ping(MsgType type, const PingMsg& m, BufferPool& pool = BufferPool::global());
OutFrame encode_json(MsgType type, const nlohmann::json& body, std::uint32_t correlation = 0, bool response = false,
                     BufferPool& pool = BufferPool::global());
OutFrame encode_error(ErrorCode code, std::string_view message, std::uint32_t correlation = 0,
                      BufferPool& pool = BufferPool::global());

// Decoders validate every length against the payload; spans point into `payload`.
Status decode_submit(ByteSpan payload, UsbSubmitMsg& out);
Status decode_complete(ByteSpan payload, UsbCompleteMsg& out);
Status decode_cancel(ByteSpan payload, UsbCancelMsg& out);
Status decode_reset(ByteSpan payload, UsbResetMsg& out);
Status decode_config(ByteSpan payload, UsbConfigMsg& out);
Status decode_op_result(ByteSpan payload, UsbOpResultMsg& out);
Status decode_ping(ByteSpan payload, PingMsg& out);
Result<nlohmann::json> decode_json(const InFrame& f);

// ----------------------------------------------------------- stream decoder

// Incremental decoder for a byte stream. Usage:
//   auto buf = dec.prepare();        // where to read the next bytes
//   n = socket.read_some(buf);
//   dec.commit(n, [&](InFrame&& f) { ... });   // may emit 0..k frames
// Large payloads are read directly into the final frame buffer (no extra copy).
class FrameDecoder {
public:
    explicit FrameDecoder(std::uint32_t max_payload = kDefaultMaxPayload, std::size_t staging_size = 64 * 1024,
                          BufferPool& pool = BufferPool::global());

    using FrameHandler = std::function<void(InFrame&&)>;

    MutableByteSpan prepare();
    // Accounts n bytes written into the span returned by prepare(); emits complete
    // frames. Returns an error on protocol violation (connection must be closed).
    Status commit(std::size_t n, const FrameHandler& on_frame);
    void set_max_payload(std::uint32_t m) noexcept { max_payload_ = m; }

private:
    Status parse_staging(const FrameHandler& on_frame);

    std::uint32_t max_payload_;
    BufferPool& pool_;
    Bytes staging_;
    std::size_t staging_begin_ = 0;
    std::size_t staging_end_ = 0;
    // frame currently being filled directly
    bool direct_ = false;
    InFrame pending_;
    std::size_t pending_filled_ = 0;
};

}  // namespace pf::proto
