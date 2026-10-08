// Licensing. Deliberately isolated from the USB data plane: limits are checked only
// when a client connects or attaches a device. LAN operation never needs Internet.
//
// License file (license.json) = JSON object + Ed25519 signature over the canonical
// (sorted-keys, compact) JSON of all other fields:
//   {"id":"…","edition":"technician","licensee":"…","device_id":"<machine id>",
//    "issued":"2026-10-01","expires":"2027-10-01","max_clients":3,"max_devices":4,
//    "signature":"<base64>"}
// Development builds (PF_DEVELOPMENT_MODE) run unlimited without a license.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "Common/Error.hpp"

namespace pf {

enum class Edition : std::uint8_t { Development = 0, Free, Professional, Technician, Enterprise };
std::string_view to_string(Edition e) noexcept;
bool parse_edition(std::string_view s, Edition& out) noexcept;

struct License {
    std::string id;
    Edition edition = Edition::Free;
    std::string licensee;
    std::string device_id;        // empty = not machine-bound
    std::int64_t issued_ms = 0;
    std::int64_t expires_ms = 0;  // 0 = perpetual
    std::uint32_t max_clients = 1;
    std::uint32_t max_devices = 1;
};

// Default limits per edition when the license does not override them.
License default_license(Edition e);

class LicenseManager {
public:
    // Loads `file`; falls back to Development (dev builds) or Free (production).
    Status load(const std::filesystem::path& file);
    [[nodiscard]] License current() const;
    [[nodiscard]] bool allows_clients(std::uint32_t count) const;
    [[nodiscard]] bool allows_devices(std::uint32_t count) const;
    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] std::string status_message() const;

    // Verifies a license document against a PEM Ed25519 public key.
    static Result<License> verify(std::string_view json_text, std::string_view public_key_pem,
                                  std::string_view machine_id, std::int64_t now_ms);
    // Signs (used by Tools/pflicense; the private key never ships with the product).
    static Result<std::string> sign(const nlohmann::json& unsigned_doc, std::string_view private_key_pem);
    // Stable, privacy-preserving machine id: SHA-256 of the OS machine GUID, 16 hex.
    static std::string machine_id();
    static std::string_view embedded_public_key();

private:
    mutable std::mutex m_;
    License lic_ = default_license(Edition::Free);
    std::string status_;
};

}  // namespace pf
