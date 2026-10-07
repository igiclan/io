// Size-class buffer pool used for frames and USB payloads.
//
// Rationale: the data plane allocates one buffer per URB in each direction. With
// pooling, steady-state transfer performs no heap allocation. Memory held by idle
// pools is bounded by max_cached_bytes and released by trim() (called when idle).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace pf {

class BufferPool;

class PooledBuffer {
public:
    PooledBuffer() = default;
    PooledBuffer(const PooledBuffer&) = delete;
    PooledBuffer& operator=(const PooledBuffer&) = delete;
    PooledBuffer(PooledBuffer&& o) noexcept { swap(o); }
    PooledBuffer& operator=(PooledBuffer&& o) noexcept {
        if (this != &o) { reset(); swap(o); }
        return *this;
    }
    ~PooledBuffer() { reset(); }

    [[nodiscard]] std::uint8_t* data() noexcept { return data_; }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return data_ != nullptr; }

    // Sets the logical size; must not exceed capacity.
    void resize(std::size_t n) noexcept { size_ = n <= capacity_ ? n : capacity_; }
    [[nodiscard]] std::span<std::uint8_t> span() noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<const std::uint8_t> span() const noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<std::uint8_t> full() noexcept { return {data_, capacity_}; }

    void reset() noexcept;

private:
    friend class BufferPool;
    PooledBuffer(std::uint8_t* d, std::size_t cap, BufferPool* p, int cls) noexcept
        : data_(d), capacity_(cap), pool_(p), cls_(cls) {}
    void swap(PooledBuffer& o) noexcept {
        std::swap(data_, o.data_);
        std::swap(capacity_, o.capacity_);
        std::swap(size_, o.size_);
        std::swap(pool_, o.pool_);
        std::swap(cls_, o.cls_);
    }

    std::uint8_t* data_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
    BufferPool* pool_ = nullptr;
    int cls_ = -1;  // -1 = unpooled heap allocation
};

struct BufferPoolStats {
    std::uint64_t acquisitions = 0;
    std::uint64_t hits = 0;           // served from cache
    std::uint64_t heap_allocations = 0;
    std::size_t cached_bytes = 0;
    std::size_t outstanding = 0;      // buffers currently handed out
};

class BufferPool {
public:
    // Size classes: 512 B .. 4 MiB (powers of 4). Larger requests go to the heap.
    static constexpr std::array<std::size_t, 8> kClassSizes = {
        512, 2048, 8192, 32768, 131072, 524288, 2097152, 4194304};

    explicit BufferPool(std::size_t max_cached_bytes = 64u * 1024 * 1024);
    ~BufferPool();
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // Returns a buffer with capacity >= min_capacity and size == min_capacity.
    PooledBuffer acquire(std::size_t min_capacity);
    // Frees all cached (idle) buffers.
    void trim();
    [[nodiscard]] BufferPoolStats stats() const;
    void set_max_cached_bytes(std::size_t n) noexcept { max_cached_.store(n); }

    // Process-wide pool. Intentionally never destroyed so buffers released during
    // static destruction remain valid.
    static BufferPool& global();

private:
    friend class PooledBuffer;
    void release(std::uint8_t* p, int cls) noexcept;

    struct SizeClass {
        std::mutex m;
        std::vector<std::uint8_t*> free;
    };
    std::array<SizeClass, kClassSizes.size()> classes_;
    std::atomic<std::size_t> max_cached_;
    std::atomic<std::size_t> cached_bytes_{0};
    std::atomic<std::uint64_t> acquisitions_{0};
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> heap_allocs_{0};
    std::atomic<std::size_t> outstanding_{0};
};

}  // namespace pf
