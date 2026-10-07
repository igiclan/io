#include "Crypto/TokenStore.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

#include "Common/Platform.hpp"
#include "Crypto/Crypto.hpp"

namespace pf {

namespace {
std::string hash_secret(const std::string& salt_hex, std::string_view secret) {
    Bytes salt;
    from_hex(salt_hex, salt);
    auto d = crypto::hmac_sha256(salt, ByteSpan(reinterpret_cast<const std::uint8_t*>(secret.data()), secret.size()));
    return to_hex(d);
}

bool parse_token(std::string_view token, std::string& id, std::string& secret) {
    if (token.size() != 3 + 8 + 1 + 64 || token.substr(0, 3) != "pf_" || token[11] != '_') return false;
    id = token.substr(3, 8);
    secret = token.substr(12);
    Bytes tmp;
    return from_hex(id, tmp) && from_hex(secret, tmp);
}
}  // namespace

Status TokenStore::load() {
    std::lock_guard lk(m_);
    records_.clear();
    std::error_code ec;
    if (file_.empty() || !std::filesystem::exists(file_, ec)) return {};
    std::ifstream in(file_);
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("tokens"))
        return {ErrorCode::MalformedPayload, "invalid token store " + file_.string()};
    try {
        for (const auto& t : j["tokens"]) {
            TokenRecord r;
            r.id = t.at("id").get<std::string>();
            r.name = t.value("name", "");
            r.salt_hex = t.at("salt").get<std::string>();
            r.hash_hex = t.at("hash").get<std::string>();
            r.admin = t.value("admin", false);
            r.disabled = t.value("disabled", false);
            r.created_ms = t.value("created", std::int64_t{0});
            r.last_used_ms = t.value("last_used", std::int64_t{0});
            r.allowed_devices = t.value("allowed_devices", std::vector<std::string>{});
            records_.push_back(std::move(r));
        }
    } catch (const nlohmann::json::exception& e) {
        return {ErrorCode::MalformedPayload, std::string("token store: ") + e.what()};
    }
    return {};
}

Status TokenStore::save() const {
    if (file_.empty()) return {};
    nlohmann::json arr = nlohmann::json::array();
    {
        std::lock_guard lk(m_);
        for (const auto& r : records_) {
            arr.push_back({{"id", r.id},
                           {"name", r.name},
                           {"salt", r.salt_hex},
                           {"hash", r.hash_hex},
                           {"admin", r.admin},
                           {"disabled", r.disabled},
                           {"created", r.created_ms},
                           {"last_used", r.last_used_ms},
                           {"allowed_devices", r.allowed_devices}});
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    auto tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return {ErrorCode::Io, "cannot write " + tmp.string()};
        out << nlohmann::json{{"version", 1}, {"tokens", arr}}.dump(2) << '\n';
        if (!out) return {ErrorCode::Io, "write failed " + tmp.string()};
    }
    (void)crypto::restrict_file_permissions(tmp);
    std::filesystem::rename(tmp, file_, ec);
    if (ec) return {ErrorCode::Io, "rename failed: " + ec.message()};
    return {};
}

Result<std::string> TokenStore::create(std::string name, bool admin, std::vector<std::string> allowed_devices) {
    TokenRecord r;
    {
        std::lock_guard lk(m_);
        do {
            r.id = crypto::random_hex(4);
        } while (std::any_of(records_.begin(), records_.end(), [&](const TokenRecord& x) { return x.id == r.id; }));
    }
    const std::string secret = crypto::random_hex(32);
    r.name = std::move(name);
    r.salt_hex = crypto::random_hex(16);
    r.hash_hex = hash_secret(r.salt_hex, secret);
    r.admin = admin;
    r.created_ms = unix_time_ms();
    r.allowed_devices = std::move(allowed_devices);
    const std::string token = "pf_" + r.id + "_" + secret;
    {
        std::lock_guard lk(m_);
        records_.push_back(std::move(r));
    }
    if (auto st = save(); !st) return st.error();
    return token;
}

Status TokenStore::revoke(const std::string& id) {
    {
        std::lock_guard lk(m_);
        auto it = std::find_if(records_.begin(), records_.end(), [&](const TokenRecord& r) { return r.id == id; });
        if (it == records_.end()) return {ErrorCode::NotFound, "token " + id};
        records_.erase(it);
    }
    return save();
}

Result<TokenIdentity> TokenStore::verify(std::string_view token) {
    std::string id, secret;
    if (!parse_token(token, id, secret)) return Error{ErrorCode::AuthFailed, "malformed token"};
    std::lock_guard lk(m_);
    for (auto& r : records_) {
        if (r.id != id) continue;
        const std::string h = hash_secret(r.salt_hex, secret);
        if (!crypto::constant_time_equal(h, r.hash_hex) || r.disabled) break;
        r.last_used_ms = unix_time_ms();
        return TokenIdentity{r.id, r.name, r.admin, r.allowed_devices};
    }
    return Error{ErrorCode::AuthFailed, "invalid token"};
}

std::vector<TokenRecord> TokenStore::list() const {
    std::lock_guard lk(m_);
    return records_;
}

bool TokenStore::empty() const {
    std::lock_guard lk(m_);
    return records_.empty();
}

bool TokenStore::glob_match(std::string_view p, std::string_view t) {
    std::size_t pi = 0, ti = 0, star = std::string_view::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || std::tolower(static_cast<unsigned char>(p[pi])) ==
                                                   std::tolower(static_cast<unsigned char>(t[ti])))) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

}  // namespace pf
