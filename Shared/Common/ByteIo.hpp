// Bounds-checked little-endian serialization helpers used by the binary protocol.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace pf {

using Bytes = std::vector<std::uint8_t>;
using ByteSpan = std::span<const std::uint8_t>;
using MutableByteSpan = std::span<std::uint8_t>;

inline void store_le16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void store_le32(std::uint8_t* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void store_le64(std::uint8_t* p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline std::uint16_t load_le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t load_le32(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline std::uint64_t load_le64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// Appends to a caller supplied byte vector.
class ByteWriter {
public:
    explicit ByteWriter(Bytes& out) : out_(out) {}

    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16(std::uint16_t v) { store_le16(grow(2), v); }
    void u32(std::uint32_t v) { store_le32(grow(4), v); }
    void u64(std::uint64_t v) { store_le64(grow(8), v); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void bytes(ByteSpan s) { out_.insert(out_.end(), s.begin(), s.end()); }
    void zeros(std::size_t n) { out_.insert(out_.end(), n, 0); }
    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }

private:
    std::uint8_t* grow(std::size_t n) {
        const std::size_t at = out_.size();
        out_.resize(at + n);
        return out_.data() + at;
    }
    Bytes& out_;
};

// Writes into a fixed span; overflow sets the failed flag instead of writing.
class SpanWriter {
public:
    explicit SpanWriter(MutableByteSpan out) : out_(out) {}
    void u8(std::uint8_t v) { if (need(1)) out_[pos_++] = v; }
    void u16(std::uint16_t v) { if (need(2)) { store_le16(&out_[pos_], v); pos_ += 2; } }
    void u32(std::uint32_t v) { if (need(4)) { store_le32(&out_[pos_], v); pos_ += 4; } }
    void u64(std::uint64_t v) { if (need(8)) { store_le64(&out_[pos_], v); pos_ += 8; } }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void bytes(ByteSpan s) {
        if (need(s.size()) && !s.empty()) { std::memcpy(&out_[pos_], s.data(), s.size()); pos_ += s.size(); }
    }
    void zeros(std::size_t n) { if (need(n)) { std::memset(&out_[pos_], 0, n); pos_ += n; } }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] bool ok() const noexcept { return !failed_; }

private:
    bool need(std::size_t n) {
        if (failed_ || out_.size() - pos_ < n) { failed_ = true; return false; }
        return true;
    }
    MutableByteSpan out_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

// Reads from a span; any out-of-bounds read sets the failed flag and yields 0.
class ByteReader {
public:
    explicit ByteReader(ByteSpan in) : in_(in) {}

    std::uint8_t u8() { return need(1) ? in_[pos_++] : 0; }
    std::uint16_t u16() { if (!need(2)) return 0; auto v = load_le16(&in_[pos_]); pos_ += 2; return v; }
    std::uint32_t u32() { if (!need(4)) return 0; auto v = load_le32(&in_[pos_]); pos_ += 4; return v; }
    std::uint64_t u64() { if (!need(8)) return 0; auto v = load_le64(&in_[pos_]); pos_ += 8; return v; }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    ByteSpan bytes(std::size_t n) {
        if (!need(n)) return {};
        auto s = in_.subspan(pos_, n);
        pos_ += n;
        return s;
    }
    void skip(std::size_t n) { if (need(n)) pos_ += n; }
    [[nodiscard]] std::size_t remaining() const noexcept { return failed_ ? 0 : in_.size() - pos_; }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] bool ok() const noexcept { return !failed_; }

private:
    bool need(std::size_t n) {
        if (failed_ || in_.size() - pos_ < n) { failed_ = true; return false; }
        return true;
    }
    ByteSpan in_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

std::string to_hex(ByteSpan data);
bool from_hex(std::string_view hex, Bytes& out);
std::string base64_encode(ByteSpan data);
bool base64_decode(std::string_view in, Bytes& out);

}  // namespace pf
