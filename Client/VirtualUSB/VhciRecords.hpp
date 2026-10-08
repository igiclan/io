// Encoding/decoding of the pfvhci user/kernel ABI (Shared/Driver/pfvhci_ioctl.h).
// Used by the Windows VHCI bridge and by the test double that stands in for the
// kernel driver in automated tests, so the byte format is verified in user mode.
#pragma once

#include <cstdint>
#include <vector>

#include "Common/ByteIo.hpp"
#include "Common/Error.hpp"
#include "Models/Descriptors.hpp"
#include "Protocol/Protocol.hpp"

namespace pf::client {

enum class VhciRecordKind : std::uint16_t {
    Submit = 0x0001,
    Cancel = 0x0002,
    EndpointReset = 0x0003,
    SelectConfig = 0x0004,
    SelectInterface = 0x0005,
    DeviceReset = 0x0006,
    DeviceRemoved = 0x0007,
};

// A record fetched from the driver (kernel -> user).
struct VhciRequest {
    VhciRecordKind kind = VhciRecordKind::Submit;
    std::uint32_t device_handle = 0;
    std::uint64_t cookie = 0;
    std::uint64_t request_id = 0;
    // Submit
    std::uint8_t endpoint = 0;
    TransferType type = TransferType::Bulk;
    std::uint16_t flags = 0;
    std::uint32_t length = 0;
    std::uint32_t timeout_ms = 0;
    std::array<std::uint8_t, 8> setup{};
    std::vector<proto::IsoPacket> iso;
    ByteSpan out_data;  // points into the fetch buffer (valid until the next fetch)
    // EndpointReset / SelectConfig / SelectInterface
    std::uint8_t op_endpoint = 0;
    std::uint8_t config_value = 0;
    std::uint8_t interface_number = 0;
    std::uint8_t alt_setting = 0;
    [[nodiscard]] bool is_in() const noexcept { return (flags & xfer_flags::kDirIn) != 0; }
};

// A completion (user -> kernel).
struct VhciCompletion {
    bool is_op_result = false;  // OP_RESULT instead of COMPLETION
    std::uint32_t device_handle = 0;
    std::uint64_t cookie = 0;
    std::uint64_t request_id = 0;
    UsbStatus status = UsbStatus::Success;
    std::uint32_t actual_length = 0;
    std::uint32_t iso_error_count = 0;
    std::vector<proto::IsoResult> iso;
    ByteSpan in_data;
};

// IOCTL_PFVHCI_PLUGIN input buffer.
Bytes encode_plugin(const DescriptorSet& set, UsbSpeed speed, std::uint64_t cookie);
// Driver-side validation/decoding of a PLUGIN buffer (test double + unit tests).
Result<DescriptorSet> decode_plugin(ByteSpan buf, UsbSpeed& speed, std::uint64_t& cookie);

// Parses a FETCH output buffer into records. Spans point into `buf`.
Result<std::vector<VhciRequest>> parse_fetch_buffer(ByteSpan buf);
// Encodes one completion record, appending to `out` (8-byte aligned).
void append_completion(Bytes& out, const VhciCompletion& c);

// --- driver side (used only by the test double) ---
void append_request(Bytes& out, const VhciRequest& r);
// in_data of each completion = everything after the iso results (incl. padding);
// for IN transfers the first actual_length bytes are the data.
Result<std::vector<VhciCompletion>> parse_complete_buffer(ByteSpan buf);

}  // namespace pf::client
