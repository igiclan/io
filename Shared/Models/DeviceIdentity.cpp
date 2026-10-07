#include "Models/DeviceIdentity.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace pf {

std::uint64_t fnv1a64(const void* data, std::size_t n, std::uint64_t seed) noexcept {
    auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

bool is_placeholder_serial(const std::string& serial) {
    if (serial.empty()) return true;
    std::string s;
    for (char c : serial) s += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const char* kKnown[] = {"0123456789ABCDEF", "0123456789", "1234567890ABCDEF", "123456789ABCDEF",
                                   "0000000000000000", "FFFFFFFFFFFFFFFF", "NONE", "N/A", "SERIAL",
                                   "0123456789ABCDEF0", "1234567890"};
    for (const char* k : kKnown) {
        if (s == k) return true;
    }
    // all identical characters ("00000000", "AAAAAAA")
    if (s.size() >= 4 && std::all_of(s.begin(), s.end(), [&](char c) { return c == s[0]; })) return true;
    return false;
}

IdentityMatch IdentityMatcher::match(const DeviceIdentity& prev, std::int64_t removed_at_ms,
                                     const DeviceIdentity& cand, std::int64_t now_ms) const {
    IdentityMatch m;
    const std::int64_t age = now_ms - removed_at_ms;
    if (age < 0 || age > policy_.max_age_ms) {
        m.reasons.emplace_back("removed too long ago");
        return m;
    }
    const bool prev_serial = !is_placeholder_serial(prev.serial);
    const bool cand_serial = !is_placeholder_serial(cand.serial);
    if (prev_serial && cand_serial) {
        if (prev.serial == cand.serial) {
            m.score += policy_.serial_equal;
            m.reasons.emplace_back("serial equal");
        } else {
            m.score += policy_.serial_conflict;
            m.reasons.emplace_back("serial differs");
        }
    }
    if (!prev.port_path.empty() && !cand.port_path.empty()) {
        if (prev.port_path == cand.port_path) {
            m.score += policy_.port_equal;
            m.reasons.emplace_back("same physical port");
        } else {
            m.score += policy_.port_conflict;
            m.reasons.emplace_back("different physical port");
        }
    }
    if (!prev.parent_id.empty() && prev.parent_id == cand.parent_id) {
        m.score += policy_.parent_equal;
        m.reasons.emplace_back("same parent hub");
    }
    if (!prev.container_id.empty() && prev.container_id == cand.container_id) {
        m.score += policy_.container_equal;
        m.reasons.emplace_back("same container id");
    }
    if (prev.vid == cand.vid && prev.vid != 0) {
        m.score += policy_.vid_equal;
        m.reasons.emplace_back("same vendor");
    }
    if (age <= policy_.recent_ms) {
        m.score += policy_.recent_bonus;
        m.reasons.emplace_back("re-appeared quickly");
    }
    m.matched = m.score >= policy_.threshold;
    return m;
}

std::string make_device_id(const DeviceIdentity& id) {
    std::string key;
    if (!id.port_path.empty()) {
        key = "port:" + id.port_path;
    } else if (!id.instance_id.empty()) {
        key = "inst:" + id.instance_id;
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%u:%u", id.bus, id.address);
        key = buf;
    }
    char vp[16];
    std::snprintf(vp, sizeof(vp), "|%04x:%04x", id.vid, id.pid);
    key += vp;
    key += "|" + id.serial;
    std::uint64_t h = fnv1a64(key.data(), key.size());
    char out[20];
    std::snprintf(out, sizeof(out), "d%012llx", static_cast<unsigned long long>(h & 0xFFFFFFFFFFFFULL));
    return out;
}

}  // namespace pf
