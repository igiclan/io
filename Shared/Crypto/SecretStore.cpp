#include "Crypto/SecretStore.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

#include "Common/ByteIo.hpp"
#include "Crypto/Crypto.hpp"

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#endif

namespace pf {

namespace {
const char kEntropy[] = "ParsFlash USB Redirector secret v1";

Result<Bytes> protect(const std::string& plain) {
#ifdef _WIN32
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB ent{static_cast<DWORD>(sizeof(kEntropy)), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"ParsFlash", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return Error{ErrorCode::Internal, "CryptProtectData failed: " + std::to_string(GetLastError())};
    Bytes b(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return b;
#else
    return Bytes(plain.begin(), plain.end());
#endif
}

Result<std::string> unprotect(const Bytes& blob) {
#ifdef _WIN32
    DATA_BLOB in{static_cast<DWORD>(blob.size()), const_cast<BYTE*>(blob.data())};
    DATA_BLOB ent{static_cast<DWORD>(sizeof(kEntropy)), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return Error{ErrorCode::AccessDenied, "CryptUnprotectData failed: " + std::to_string(GetLastError())};
    std::string s(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
#else
    return std::string(blob.begin(), blob.end());
#endif
}
}  // namespace

bool SecretStore::is_encrypted_at_rest() noexcept {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

Status SecretStore::load() {
    std::lock_guard lk(m_);
    blobs_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) return {};
    std::ifstream in(file_);
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return {ErrorCode::MalformedPayload, "invalid secret store"};
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_string()) blobs_[it.key()] = it.value().get<std::string>();
    }
    return {};
}

Status SecretStore::save_locked() const {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : blobs_) j[k] = v;
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    auto tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return {ErrorCode::Io, "cannot write " + tmp.string()};
        out << j.dump(2);
    }
    (void)crypto::restrict_file_permissions(tmp);
    std::filesystem::rename(tmp, file_, ec);
    if (ec) return {ErrorCode::Io, ec.message()};
    return {};
}

Status SecretStore::put(const std::string& key, const std::string& secret) {
    auto blob = protect(secret);
    if (!blob) return blob.error();
    std::lock_guard lk(m_);
    blobs_[key] = base64_encode(*blob);
    return save_locked();
}

Result<std::string> SecretStore::get(const std::string& key) const {
    std::string b64;
    {
        std::lock_guard lk(m_);
        auto it = blobs_.find(key);
        if (it == blobs_.end()) return Error{ErrorCode::NotFound, "secret " + key};
        b64 = it->second;
    }
    Bytes blob;
    if (!base64_decode(b64, blob)) return Error{ErrorCode::MalformedPayload, "secret encoding"};
    return unprotect(blob);
}

Status SecretStore::erase(const std::string& key) {
    std::lock_guard lk(m_);
    blobs_.erase(key);
    return save_locked();
}

}  // namespace pf
