// API / client tokens.
//
// Token format: "pf_<id>_<secret>" where <id> is 8 hex chars (public lookup key) and
// <secret> is 64 hex chars (256 random bits). Only HMAC-SHA256(salt, secret) is
// persisted. Because the secret is a full-entropy random value (not a human
// password), a fast keyed hash is sufficient; slow KDFs protect low-entropy
// passwords, which tokens are not. The plaintext token is shown exactly once, when
// created.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Common/Error.hpp"

namespace pf {

struct TokenRecord {
    std::string id;
    std::string name;            // human label, e.g. "Repair bench 3"
    std::string salt_hex;
    std::string hash_hex;
    bool admin = false;          // may use management API write operations / force detach
    bool disabled = false;
    std::int64_t created_ms = 0;
    std::int64_t last_used_ms = 0;
    std::vector<std::string> allowed_devices;  // glob patterns on "vid:pid", device id or port; empty = all
};

struct TokenIdentity {
    std::string id;
    std::string name;
    bool admin = false;
    std::vector<std::string> allowed_devices;
};

class TokenStore {
public:
    TokenStore() = default;
    explicit TokenStore(std::filesystem::path file) : file_(std::move(file)) {}

    void set_file(std::filesystem::path file) {
        std::lock_guard lk(m_);
        file_ = std::move(file);
    }
    Status load();  // missing file = empty store
    Status save() const;

    // Creates a token; returns the plaintext token (only time it is available).
    Result<std::string> create(std::string name, bool admin, std::vector<std::string> allowed_devices = {});
    Status revoke(const std::string& id);
    Result<TokenIdentity> verify(std::string_view token);
    [[nodiscard]] std::vector<TokenRecord> list() const;
    [[nodiscard]] bool empty() const;

    // Glob match ('*' and '?') used for allowed_devices.
    static bool glob_match(std::string_view pattern, std::string_view text);

private:
    std::filesystem::path file_;
    mutable std::mutex m_;
    std::vector<TokenRecord> records_;
};

}  // namespace pf
