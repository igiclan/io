#include "Common/Platform.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fstream>
#endif

namespace pf {

std::uint64_t monotonic_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::int64_t unix_time_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string iso8601_utc(std::int64_t unix_ms) {
    std::time_t secs = static_cast<std::time_t>(unix_ms / 1000);
    int ms = static_cast<int>(unix_ms % 1000);
    if (ms < 0) ms += 1000;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return buf;
}

std::string host_name() {
#ifdef _WIN32
    char buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = sizeof(buf);
    if (GetComputerNameA(buf, &n)) return std::string(buf, n);
    return "unknown";
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = 0;
        return buf;
    }
    return "unknown";
#endif
}

std::filesystem::path app_data_dir(std::string_view app) {
    namespace fs = std::filesystem;
    fs::path base;
    if (const char* home = std::getenv("PARSFLASH_HOME"); home && *home) {
        base = fs::path(home);
    } else {
#ifdef _WIN32
        const char* pd = std::getenv("ProgramData");
        base = fs::path(pd ? pd : "C:\\ProgramData") / "ParsFlash";
#else
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
            base = fs::path(xdg) / "parsflash";
        } else if (const char* h = std::getenv("HOME"); h && *h) {
            base = fs::path(h) / ".config" / "parsflash";
        } else {
            base = fs::path("/tmp") / "parsflash";
        }
#endif
    }
    return base / std::string(app);
}

ProcessUsage sample_process_usage() {
    ProcessUsage u;
    u.wall_ns = monotonic_ns();
#ifdef _WIN32
    FILETIME creation, exit_t, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_t, &kernel, &user)) {
        auto to_ns = [](const FILETIME& ft) {
            ULARGE_INTEGER v;
            v.LowPart = ft.dwLowDateTime;
            v.HighPart = ft.dwHighDateTime;
            return static_cast<std::uint64_t>(v.QuadPart) * 100;
        };
        u.cpu_time_ns = to_ns(kernel) + to_ns(user);
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        u.rss_bytes = pmc.WorkingSetSize;
    }
#else
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        u.cpu_time_ns = (static_cast<std::uint64_t>(ru.ru_utime.tv_sec) + ru.ru_stime.tv_sec) * 1000000000ULL +
                        (static_cast<std::uint64_t>(ru.ru_utime.tv_usec) + ru.ru_stime.tv_usec) * 1000ULL;
    }
    std::ifstream statm("/proc/self/statm");
    std::uint64_t size_pages = 0, rss_pages = 0;
    if (statm >> size_pages >> rss_pages) {
        u.rss_bytes = rss_pages * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    }
#endif
    return u;
}

unsigned hardware_threads() noexcept {
    unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
}

double CpuMeter::sample() {
    ProcessUsage now = sample_process_usage();
    double pct = 0.0;
    if (has_last_ && now.wall_ns > last_.wall_ns) {
        pct = 100.0 * static_cast<double>(now.cpu_time_ns - last_.cpu_time_ns) /
              static_cast<double>(now.wall_ns - last_.wall_ns);
    }
    last_ = now;
    has_last_ = true;
    return pct;
}

void set_current_thread_name(const char* name) noexcept {
#ifdef _WIN32
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static auto fn = reinterpret_cast<SetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (fn) {
        wchar_t w[64];
        int i = 0;
        for (; name[i] && i < 63; ++i) w[i] = static_cast<wchar_t>(name[i]);
        w[i] = 0;
        fn(GetCurrentThread(), w);
    }
#elif defined(__linux__)
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s", name);
    pthread_setname_np(pthread_self(), buf);
#else
    (void)name;
#endif
}

}  // namespace pf
