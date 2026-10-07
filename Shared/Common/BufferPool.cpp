#include "Common/BufferPool.hpp"

#include <new>

namespace pf {

void PooledBuffer::reset() noexcept {
    if (data_ == nullptr) return;
    if (pool_ != nullptr) {
        pool_->release(data_, cls_);
    } else {
        ::operator delete(data_);
    }
    data_ = nullptr;
    capacity_ = 0;
    size_ = 0;
    pool_ = nullptr;
    cls_ = -1;
}

BufferPool::BufferPool(std::size_t max_cached_bytes) : max_cached_(max_cached_bytes) {}

BufferPool::~BufferPool() { trim(); }

BufferPool& BufferPool::global() {
    static BufferPool* pool = new BufferPool();  // leaked on purpose
    return *pool;
}

PooledBuffer BufferPool::acquire(std::size_t min_capacity) {
    acquisitions_.fetch_add(1, std::memory_order_relaxed);
    if (min_capacity == 0) min_capacity = 1;
    for (std::size_t i = 0; i < kClassSizes.size(); ++i) {
        if (min_capacity > kClassSizes[i]) continue;
        auto& sc = classes_[i];
        std::uint8_t* p = nullptr;
        {
            std::lock_guard lk(sc.m);
            if (!sc.free.empty()) {
                p = sc.free.back();
                sc.free.pop_back();
            }
        }
        if (p != nullptr) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            cached_bytes_.fetch_sub(kClassSizes[i], std::memory_order_relaxed);
        } else {
            heap_allocs_.fetch_add(1, std::memory_order_relaxed);
            p = static_cast<std::uint8_t*>(::operator new(kClassSizes[i]));
        }
        outstanding_.fetch_add(1, std::memory_order_relaxed);
        PooledBuffer b(p, kClassSizes[i], this, static_cast<int>(i));
        b.resize(min_capacity);
        return b;
    }
    // Oversized: plain heap buffer, not cached.
    heap_allocs_.fetch_add(1, std::memory_order_relaxed);
    auto* p = static_cast<std::uint8_t*>(::operator new(min_capacity));
    PooledBuffer b(p, min_capacity, nullptr, -1);
    b.resize(min_capacity);
    return b;
}

void BufferPool::release(std::uint8_t* p, int cls) noexcept {
    outstanding_.fetch_sub(1, std::memory_order_relaxed);
    const std::size_t sz = kClassSizes[static_cast<std::size_t>(cls)];
    if (cached_bytes_.load(std::memory_order_relaxed) + sz <= max_cached_.load(std::memory_order_relaxed)) {
        auto& sc = classes_[static_cast<std::size_t>(cls)];
        std::lock_guard lk(sc.m);
        try {
            sc.free.push_back(p);
            cached_bytes_.fetch_add(sz, std::memory_order_relaxed);
            return;
        } catch (...) {
            // fall through to delete
        }
    }
    ::operator delete(p);
}

void BufferPool::trim() {
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        std::vector<std::uint8_t*> victims;
        {
            std::lock_guard lk(classes_[i].m);
            victims.swap(classes_[i].free);
        }
        cached_bytes_.fetch_sub(victims.size() * kClassSizes[i], std::memory_order_relaxed);
        for (auto* p : victims) ::operator delete(p);
    }
}

BufferPoolStats BufferPool::stats() const {
    BufferPoolStats s;
    s.acquisitions = acquisitions_.load(std::memory_order_relaxed);
    s.hits = hits_.load(std::memory_order_relaxed);
    s.heap_allocations = heap_allocs_.load(std::memory_order_relaxed);
    s.cached_bytes = cached_bytes_.load(std::memory_order_relaxed);
    s.outstanding = outstanding_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace pf
