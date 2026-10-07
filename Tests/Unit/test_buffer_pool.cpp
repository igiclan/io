#include <doctest/doctest.h>

#include "Common/BufferPool.hpp"

using namespace pf;

TEST_CASE("BufferPool reuses buffers of the same size class") {
    BufferPool pool(1024 * 1024);
    std::uint8_t* first = nullptr;
    {
        auto b = pool.acquire(1000);
        CHECK(b.size() == 1000);
        CHECK(b.capacity() == 2048);
        first = b.data();
    }
    auto s = pool.stats();
    CHECK(s.outstanding == 0);
    CHECK(s.cached_bytes == 2048);
    auto b2 = pool.acquire(1500);
    CHECK(b2.data() == first);
    CHECK(pool.stats().hits == 1);
}

TEST_CASE("BufferPool respects cache bound and trims") {
    BufferPool pool(4096);
    {
        auto a = pool.acquire(2048);
        auto b = pool.acquire(2048);
        auto c = pool.acquire(2048);
    }
    CHECK(pool.stats().cached_bytes <= 4096);
    pool.trim();
    CHECK(pool.stats().cached_bytes == 0);
}

TEST_CASE("Oversized buffers bypass the pool") {
    BufferPool pool;
    auto big = pool.acquire(BufferPool::kClassSizes.back() + 1);
    CHECK(big.size() == BufferPool::kClassSizes.back() + 1);
    big.reset();
    CHECK(pool.stats().cached_bytes == 0);
}

TEST_CASE("PooledBuffer move semantics") {
    BufferPool pool;
    auto a = pool.acquire(10);
    auto* p = a.data();
    PooledBuffer b = std::move(a);
    CHECK(b.data() == p);
    CHECK_FALSE(static_cast<bool>(a));
    b = pool.acquire(20);
    CHECK(pool.stats().outstanding == 1);
}
