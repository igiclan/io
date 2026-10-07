// Thin platform abstraction: clocks, paths, host info, process resource usage.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace pf {

// Monotonic clock in nanoseconds (not related to wall time).
std::uint64_t monotonic_ns() noexcept;
// Wall clock, milliseconds since Unix epoch (UTC).
std::int64_t unix_time_ms() noexcept;
// "2026-10-07T12:34:56.789Z"
std::string iso8601_utc(std::int64_t unix_ms);

std::string host_name();

// Per-machine directory for configuration (Windows: %ProgramData%\ParsFlash\<app>,
// otherwise $XDG_CONFIG_HOME/parsflash/<app> or ~/.config/parsflash/<app>).
// Overridable via PARSFLASH_HOME environment variable (used by tests).
std::filesystem::path app_data_dir(std::string_view app);

struct ProcessUsage {
    std::uint64_t cpu_time_ns = 0;   // user + kernel
    std::uint64_t rss_bytes = 0;     // working set / resident set
    std::uint64_t wall_ns = 0;       // monotonic timestamp of the sample
};
ProcessUsage sample_process_usage();
unsigned hardware_threads() noexcept;

// Computes CPU% (of one core = 100%) between consecutive samples.
class CpuMeter {
public:
    double sample();  // returns percent since previous call (0 on first call)
private:
    ProcessUsage last_{};
    bool has_last_ = false;
};

void set_current_thread_name(const char* name) noexcept;

}  // namespace pf
