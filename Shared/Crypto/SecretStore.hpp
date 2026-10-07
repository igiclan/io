// Storage for secrets the client must be able to reuse (server tokens).
// Windows: DPAPI (CryptProtectData, per-user scope) - ciphertext only on disk.
// Other OSes: stored base64 in a 0600 file; documented in SECURITY.md.
#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>

#include "Common/Error.hpp"

namespace pf {

class SecretStore {
public:
    explicit SecretStore(std::filesystem::path file) : file_(std::move(file)) {}

    Status load();
    Status put(const std::string& key, const std::string& secret);
    Result<std::string> get(const std::string& key) const;
    Status erase(const std::string& key);
    [[nodiscard]] static bool is_encrypted_at_rest() noexcept;

private:
    Status save_locked() const;
    std::filesystem::path file_;
    mutable std::mutex m_;
    std::map<std::string, std::string> blobs_;  // key -> base64(protected bytes)
};

}  // namespace pf
