// Helpers shared by tests: temporary directories, polling waits.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "Crypto/Crypto.hpp"

namespace pf::test {

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() / ("pftest-" + crypto::random_hex(6));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(const std::string& s) const { return path_ / s; }

private:
    std::filesystem::path path_;
};

// Polls `pred` until true or timeout. Returns pred().
inline bool wait_for(const std::function<bool()>& pred, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

}  // namespace pf::test
