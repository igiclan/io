#include "Licensing/License.hpp"

#include <ctime>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include "Common/ByteIo.hpp"
#include "Common/Platform.hpp"
#include "Crypto/Crypto.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace pf {

// Public half of the license signing key. The value below is the DEVELOPMENT key
// (private key: Tools/licensing/dev_private_key.pem, for testing only). Production
// builds MUST replace it with the ParsFlash production public key via
// -DPF_LICENSE_PUBLIC_KEY_FILE=<pem> (see Docs/SECURITY.md).
#ifndef PF_LICENSE_PUBLIC_KEY
#define PF_LICENSE_PUBLIC_KEY \
    "-----BEGIN PUBLIC KEY-----\nMCowBQYDK2VwAyEAMeFt6NSKu2qYGmX/fxrZObmZJiZQqnchNj1bP4fVVlQ=\n-----END PUBLIC KEY-----\n"
#endif

std::string_view LicenseManager::embedded_public_key() { return PF_LICENSE_PUBLIC_KEY; }

std::string_view to_string(Edition e) noexcept {
    switch (e) {
        case Edition::Development: return "development";
        case Edition::Free: return "free";
        case Edition::Professional: return "professional";
        case Edition::Technician: return "technician";
        case Edition::Enterprise: return "enterprise";
    }
    return "free";
}

bool parse_edition(std::string_view s, Edition& out) noexcept {
    for (auto e : {Edition::Development, Edition::Free, Edition::Professional, Edition::Technician,
                   Edition::Enterprise}) {
        if (to_string(e) == s) {
            out = e;
            return true;
        }
    }
    return false;
}

License default_license(Edition e) {
    License l;
    l.edition = e;
    switch (e) {
        case Edition::Development: l.max_clients = 1024; l.max_devices = 1024; break;
        case Edition::Free: l.max_clients = 1; l.max_devices = 1; break;
        case Edition::Technician: l.max_clients = 2; l.max_devices = 4; break;
        case Edition::Professional: l.max_clients = 8; l.max_devices = 16; break;
        case Edition::Enterprise: l.max_clients = 1024; l.max_devices = 1024; break;
    }
    return l;
}

namespace {

std::int64_t parse_date_ms(const std::string& s) {
    // YYYY-MM-DD (UTC midnight)
    int y = 0, m = 0, d = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return -1;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
#ifdef _WIN32
    const std::time_t t = _mkgmtime(&tm);
#else
    const std::time_t t = timegm(&tm);
#endif
    return static_cast<std::int64_t>(t) * 1000;
}

std::string canonical(nlohmann::json doc) {
    doc.erase("signature");
    return doc.dump();  // nlohmann::json objects are key-sorted -> canonical
}

using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

PKeyPtr read_key(std::string_view pem, bool priv) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    EVP_PKEY* k = priv ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr)
                       : PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
    return PKeyPtr(k, EVP_PKEY_free);
}

}  // namespace

Result<License> LicenseManager::verify(std::string_view text, std::string_view pub_pem, std::string_view machine,
                                       std::int64_t now_ms) {
    auto doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return Error{ErrorCode::MalformedPayload, "license is not JSON"};
    if (!doc.contains("signature") || !doc["signature"].is_string())
        return Error{ErrorCode::AccessDenied, "license is not signed"};
    Bytes sig;
    if (!base64_decode(doc["signature"].get<std::string>(), sig)) return Error{ErrorCode::AccessDenied, "bad signature"};
    const std::string msg = canonical(doc);
    auto key = read_key(pub_pem, false);
    if (!key) return Error{ErrorCode::Internal, "invalid public key"};
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestVerify(ctx.get(), sig.data(), sig.size(), reinterpret_cast<const unsigned char*>(msg.data()),
                         msg.size()) != 1)
        return Error{ErrorCode::AccessDenied, "license signature invalid"};
    License l;
    try {
        l.id = doc.value("id", "");
        if (!parse_edition(doc.value("edition", "free"), l.edition) || l.edition == Edition::Development)
            return Error{ErrorCode::AccessDenied, "invalid edition"};
        const License defaults = default_license(l.edition);
        l.licensee = doc.value("licensee", "");
        l.device_id = doc.value("device_id", "");
        l.issued_ms = doc.contains("issued") ? parse_date_ms(doc["issued"].get<std::string>()) : 0;
        l.expires_ms = doc.contains("expires") ? parse_date_ms(doc["expires"].get<std::string>()) : 0;
        l.max_clients = doc.value("max_clients", defaults.max_clients);
        l.max_devices = doc.value("max_devices", defaults.max_devices);
    } catch (const nlohmann::json::exception& e) {
        return Error{ErrorCode::MalformedPayload, std::string("license: ") + e.what()};
    }
    if (l.expires_ms < 0 || l.issued_ms < 0) return Error{ErrorCode::MalformedPayload, "license dates"};
    if (l.expires_ms && now_ms > l.expires_ms) return Error{ErrorCode::LicenseLimit, "license expired"};
    if (!l.device_id.empty() && l.device_id != machine)
        return Error{ErrorCode::LicenseLimit, "license is bound to another machine"};
    return l;
}

Result<std::string> LicenseManager::sign(const nlohmann::json& unsigned_doc, std::string_view priv_pem) {
    auto key = read_key(priv_pem, true);
    if (!key) return Error{ErrorCode::InvalidArgument, "invalid private key"};
    const std::string msg = canonical(unsigned_doc);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    std::size_t len = 0;
    if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestSign(ctx.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) != 1)
        return Error{ErrorCode::Internal, "sign init failed"};
    Bytes sig(len);
    if (EVP_DigestSign(ctx.get(), sig.data(), &len, reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) !=
        1)
        return Error{ErrorCode::Internal, "sign failed"};
    sig.resize(len);
    nlohmann::json doc = unsigned_doc;
    doc["signature"] = base64_encode(sig);
    return doc.dump(2);
}

std::string LicenseManager::machine_id() {
    std::string raw;
#ifdef _WIN32
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &key) ==
        ERROR_SUCCESS) {
        wchar_t buf[128];
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"MachineGuid", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) ==
                ERROR_SUCCESS &&
            type == REG_SZ) {
            for (wchar_t c : std::wstring(buf)) raw += static_cast<char>(c);
        }
        RegCloseKey(key);
    }
#else
    std::ifstream in("/etc/machine-id");
    std::getline(in, raw);
#endif
    if (raw.empty()) raw = host_name();
    raw = "parsflash:" + raw;
    auto d = crypto::sha256(ByteSpan(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()));
    return to_hex(d).substr(0, 16);
}

Status LicenseManager::load(const std::filesystem::path& file) {
    std::error_code ec;
    std::lock_guard lk(m_);
    if (file.empty() || !std::filesystem::exists(file, ec)) {
#ifdef PF_DEVELOPMENT_MODE
        lic_ = default_license(Edition::Development);
        status_ = "development build: no license required";
#else
        lic_ = default_license(Edition::Free);
        status_ = "no license installed: Free edition";
#endif
        return {};
    }
    std::ifstream in(file);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    auto r = verify(text, embedded_public_key(), machine_id(), unix_time_ms());
    if (!r) {
#ifdef PF_DEVELOPMENT_MODE
        lic_ = default_license(Edition::Development);
#else
        lic_ = default_license(Edition::Free);
#endif
        status_ = "license rejected (" + r.error().describe() + "); running as " + std::string(to_string(lic_.edition));
        return r.error();
    }
    lic_ = *r;
    status_ = "licensed: " + std::string(to_string(lic_.edition));
    return {};
}

License LicenseManager::current() const {
    std::lock_guard lk(m_);
    return lic_;
}

bool LicenseManager::allows_clients(std::uint32_t n) const {
    std::lock_guard lk(m_);
    return n <= lic_.max_clients;
}

bool LicenseManager::allows_devices(std::uint32_t n) const {
    std::lock_guard lk(m_);
    return n <= lic_.max_devices;
}

std::string LicenseManager::status_message() const {
    std::lock_guard lk(m_);
    return status_;
}

nlohmann::json LicenseManager::to_json() const {
    std::lock_guard lk(m_);
    return {{"edition", std::string(pf::to_string(lic_.edition))},
            {"licensee", lic_.licensee},
            {"id", lic_.id},
            {"device_bound", !lic_.device_id.empty()},
            {"expires", lic_.expires_ms ? iso8601_utc(lic_.expires_ms) : std::string("never")},
            {"max_clients", lic_.max_clients},
            {"max_devices", lic_.max_devices},
            {"status", status_}};
}

}  // namespace pf
