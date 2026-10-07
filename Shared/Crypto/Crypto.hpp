// Cryptographic primitives (OpenSSL 3 backed).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "Common/ByteIo.hpp"
#include "Common/Error.hpp"

namespace pf::crypto {

using Sha256Digest = std::array<std::uint8_t, 32>;

// Cryptographically secure random bytes. Aborts the process if the CSPRNG fails
// (continuing without randomness would silently weaken every token).
void random_bytes(MutableByteSpan out);
std::string random_hex(std::size_t bytes);
std::string uuid4();

Sha256Digest sha256(ByteSpan data);
Sha256Digest hmac_sha256(ByteSpan key, ByteSpan data);
Bytes pbkdf2_sha256(std::string_view password, ByteSpan salt, std::uint32_t iterations, std::size_t out_len);

// Constant-time comparison (length is not secret).
bool constant_time_equal(ByteSpan a, ByteSpan b) noexcept;
bool constant_time_equal(std::string_view a, std::string_view b) noexcept;

// ---- X.509 identity -------------------------------------------------------

struct CertificateFiles {
    std::filesystem::path cert_pem;
    std::filesystem::path key_pem;
};

// Generates a self-signed ECDSA P-256 certificate (validity 10 years) when the files
// do not exist. The private key file is created with owner-only permissions.
Status ensure_self_signed_certificate(const CertificateFiles& files, std::string_view common_name);

// SHA-256 fingerprint of the DER encoding, lowercase hex with ':' separators.
Result<std::string> certificate_fingerprint_file(const std::filesystem::path& cert_pem);
std::string fingerprint_from_der(ByteSpan der);
std::string normalize_fingerprint(std::string_view fp);  // lower-case, no separators

// Restricts a file to the current user (POSIX 0600; Windows: protected DACL owner+SYSTEM+Administrators).
Status restrict_file_permissions(const std::filesystem::path& file);

}  // namespace pf::crypto
