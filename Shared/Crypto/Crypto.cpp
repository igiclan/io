#include "Crypto/Crypto.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#endif

namespace pf::crypto {

void random_bytes(MutableByteSpan out) {
    if (out.empty()) return;
    if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) {
        std::fputs("FATAL: CSPRNG failure\n", stderr);
        std::abort();
    }
}

std::string random_hex(std::size_t bytes) {
    Bytes b(bytes);
    random_bytes(b);
    return to_hex(b);
}

std::string uuid4() {
    std::uint8_t b[16];
    random_bytes(b);
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
    std::string h = to_hex(ByteSpan(b, 16));
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" +
           h.substr(20);
}

Sha256Digest sha256(ByteSpan data) {
    Sha256Digest d{};
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), d.data(), &len, EVP_sha256(), nullptr);
    return d;
}

Sha256Digest hmac_sha256(ByteSpan key, ByteSpan data) {
    Sha256Digest d{};
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data.data(), data.size(), d.data(), &len);
    return d;
}

Bytes pbkdf2_sha256(std::string_view password, ByteSpan salt, std::uint32_t iterations, std::size_t out_len) {
    Bytes out(out_len);
    PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(), static_cast<int>(salt.size()),
                      static_cast<int>(iterations), EVP_sha256(), static_cast<int>(out_len), out.data());
    return out;
}

bool constant_time_equal(ByteSpan a, ByteSpan b) noexcept {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    return constant_time_equal(ByteSpan(reinterpret_cast<const std::uint8_t*>(a.data()), a.size()),
                               ByteSpan(reinterpret_cast<const std::uint8_t*>(b.data()), b.size()));
}

// ------------------------------------------------------------- certificates

namespace {
struct FileCloser {
    void operator()(std::FILE* f) const noexcept { if (f) std::fclose(f); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;
FilePtr open_file(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
    return FilePtr(_wfopen(p.wstring().c_str(), wmode.c_str()));
#else
    return FilePtr(std::fopen(p.string().c_str(), mode));
#endif
}
}  // namespace

Status restrict_file_permissions(const std::filesystem::path& file) {
#ifdef _WIN32
    // Owner, SYSTEM and Administrators full control; inheritance disabled.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)",
                                                              SDDL_REVISION_1, &sd, nullptr))
        return {ErrorCode::Io, "ConvertStringSecurityDescriptor failed"};
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    std::wstring path = file.wstring();
    DWORD rc = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                     dacl, nullptr);
    LocalFree(sd);
    if (rc != ERROR_SUCCESS) return {ErrorCode::Io, "SetNamedSecurityInfo failed: " + std::to_string(rc)};
    return {};
#else
    if (chmod(file.string().c_str(), S_IRUSR | S_IWUSR) != 0) return {ErrorCode::Io, "chmod failed"};
    return {};
#endif
}

Status ensure_self_signed_certificate(const CertificateFiles& files, std::string_view common_name) {
    std::error_code ec;
    if (std::filesystem::exists(files.cert_pem, ec) && std::filesystem::exists(files.key_pem, ec)) return {};
    std::filesystem::create_directories(files.cert_pem.parent_path(), ec);
    std::filesystem::create_directories(files.key_pem.parent_path(), ec);

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_EC_gen("P-256"), EVP_PKEY_free);
    if (!key) return {ErrorCode::Internal, "EC key generation failed"};
    std::unique_ptr<X509, decltype(&X509_free)> x509(X509_new(), X509_free);
    if (!x509) return {ErrorCode::Internal, "X509_new failed"};
    X509_set_version(x509.get(), 2);
    std::uint8_t serial[16];
    random_bytes(serial);
    serial[0] &= 0x7F;
    std::unique_ptr<BIGNUM, decltype(&BN_free)> bn(BN_bin2bn(serial, sizeof(serial), nullptr), BN_free);
    BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(x509.get()));
    X509_gmtime_adj(X509_getm_notBefore(x509.get()), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x509.get()), 10L * 365 * 24 * 3600);
    X509_set_pubkey(x509.get(), key.get());
    X509_NAME* name = X509_get_subject_name(x509.get());
    std::string cn(common_name.empty() ? "ParsFlash USB Server" : common_name);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>("ParsFlash"), -1, -1,
                               0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1,
                               0);
    X509_set_issuer_name(x509.get(), name);
    if (X509_sign(x509.get(), key.get(), EVP_sha256()) == 0) return {ErrorCode::Internal, "X509_sign failed"};

    {
        auto kf = open_file(files.key_pem, "wb");
        if (!kf) return {ErrorCode::Io, "cannot write " + files.key_pem.string()};
        if (PEM_write_PrivateKey(kf.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
            return {ErrorCode::Io, "PEM_write_PrivateKey failed"};
    }
    if (auto st = restrict_file_permissions(files.key_pem); !st) return st;
    auto cf = open_file(files.cert_pem, "wb");
    if (!cf) return {ErrorCode::Io, "cannot write " + files.cert_pem.string()};
    if (PEM_write_X509(cf.get(), x509.get()) != 1) return {ErrorCode::Io, "PEM_write_X509 failed"};
    return {};
}

std::string fingerprint_from_der(ByteSpan der) {
    auto d = sha256(der);
    std::string hex = to_hex(d);
    std::string out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        if (i) out += ':';
        out += hex.substr(i, 2);
    }
    return out;
}

std::string normalize_fingerprint(std::string_view fp) {
    std::string out;
    for (char c : fp) {
        if (c == ':' || c == ' ' || c == '-') continue;
        out += static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
    }
    return out;
}

Result<std::string> certificate_fingerprint_file(const std::filesystem::path& cert_pem) {
    auto f = open_file(cert_pem, "rb");
    if (!f) return Error{ErrorCode::NotFound, "certificate not found: " + cert_pem.string()};
    std::unique_ptr<X509, decltype(&X509_free)> x(PEM_read_X509(f.get(), nullptr, nullptr, nullptr), X509_free);
    if (!x) return Error{ErrorCode::TlsFailure, "invalid certificate PEM"};
    int len = i2d_X509(x.get(), nullptr);
    if (len <= 0) return Error{ErrorCode::TlsFailure, "i2d_X509 failed"};
    Bytes der(static_cast<std::size_t>(len));
    unsigned char* p = der.data();
    i2d_X509(x.get(), &p);
    return fingerprint_from_der(der);
}

}  // namespace pf::crypto
