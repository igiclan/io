// Structured, asynchronous logging.
//
// Every event carries: timestamp, level, component, client, device, session,
// operation, duration, result, error code, message. Events are queued and written by
// a background thread to sinks (rotating JSON-lines file, console, in-memory ring for
// the UI/API). Logging never blocks the data plane: when the queue is full events are
// dropped and counted.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace pf {

enum class LogLevel : std::uint8_t { Trace = 0, Debug, Info, Warning, Error, Critical, Off };

std::string_view to_string(LogLevel l) noexcept;
bool parse_log_level(std::string_view s, LogLevel& out) noexcept;

struct LogEvent {
    std::int64_t timestamp_ms = 0;
    LogLevel level = LogLevel::Info;
    std::string component;
    std::string client;
    std::string device;
    std::string session;
    std::string operation;
    std::int64_t duration_us = -1;  // -1 = not applicable
    std::string result;
    std::int32_t error_code = 0;
    std::string message;
    std::uint64_t thread_id = 0;
};

nlohmann::json to_json(const LogEvent& e);
std::optional<LogEvent> log_event_from_json(const nlohmann::json& j);
std::string to_json_line(const LogEvent& e);
std::optional<LogEvent> parse_json_line(std::string_view line);
std::string format_human(const LogEvent& e);

struct LogQuery {
    LogLevel min_level = LogLevel::Trace;
    std::string component;  // exact (case-insensitive), empty = any
    std::string session;    // prefix match
    std::string device;
    std::string client;
    std::string text;       // case-insensitive substring of message/operation/result
    std::int64_t since_ms = 0;
    std::int64_t until_ms = 0;  // 0 = no upper bound
    std::size_t limit = 1000;
    [[nodiscard]] bool matches(const LogEvent& e) const;
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogEvent& e) = 0;
    virtual void flush() {}
};

class ConsoleSink final : public LogSink {
public:
    void write(const LogEvent& e) override;
};

// JSON-lines file with size-based rotation: name.log, name.1.log ... name.N.log
class RotatingFileSink final : public LogSink {
public:
    RotatingFileSink(std::filesystem::path file, std::uint64_t max_bytes = 10ull << 20, int max_files = 5);
    ~RotatingFileSink() override;
    void write(const LogEvent& e) override;
    void flush() override;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    void rotate();
    std::filesystem::path path_;
    std::uint64_t max_bytes_;
    int max_files_;
    std::FILE* f_ = nullptr;
    std::uint64_t size_ = 0;
};

// Keeps the last N events in memory for UI display and API search.
class RingBufferSink final : public LogSink {
public:
    explicit RingBufferSink(std::size_t capacity = 20000) : capacity_(capacity) {}
    void write(const LogEvent& e) override;
    [[nodiscard]] std::vector<LogEvent> query(const LogQuery& q) const;  // newest last
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex m_;
    std::deque<LogEvent> events_;
    std::size_t capacity_;
};

struct LoggerStats {
    std::uint64_t written = 0;
    std::uint64_t dropped = 0;
};

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel l) noexcept { level_.store(l, std::memory_order_relaxed); }
    [[nodiscard]] LogLevel level() const noexcept { return level_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool enabled(LogLevel l) const noexcept { return l >= level() && l != LogLevel::Off; }

    void add_sink(std::shared_ptr<LogSink> sink);
    void clear_sinks();
    void log(LogEvent e);
    void flush();     // waits until queued events are written
    void shutdown();  // stops the writer thread (idempotent)
    [[nodiscard]] LoggerStats stats() const;
    void set_queue_limit(std::size_t n) noexcept { queue_limit_ = n; }

    ~Logger();

private:
    Logger();
    void run();

    std::atomic<LogLevel> level_{LogLevel::Info};
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::condition_variable flushed_cv_;
    std::deque<LogEvent> queue_;
    std::vector<std::shared_ptr<LogSink>> sinks_;
    std::size_t queue_limit_ = 100000;
    bool stop_ = false;
    bool writing_ = false;
    std::uint64_t written_ = 0;
    std::uint64_t dropped_ = 0;
    std::thread worker_;
};

// Fluent builder: commits on destruction.
//   LogRecord(LogLevel::Info, "USB").session(sid).op("attach").result("ok").msg("attached");
class LogRecord {
public:
    LogRecord(LogLevel level, std::string_view component);
    ~LogRecord();
    LogRecord(const LogRecord&) = delete;
    LogRecord& operator=(const LogRecord&) = delete;

    LogRecord& client(std::string_view v) { if (on_) e_.client = v; return *this; }
    LogRecord& device(std::string_view v) { if (on_) e_.device = v; return *this; }
    LogRecord& session(std::string_view v) { if (on_) e_.session = v; return *this; }
    LogRecord& op(std::string_view v) { if (on_) e_.operation = v; return *this; }
    LogRecord& duration_us(std::int64_t us) { e_.duration_us = us; return *this; }
    LogRecord& result(std::string_view v) { if (on_) e_.result = v; return *this; }
    LogRecord& error(std::int32_t code) { e_.error_code = code; return *this; }
    LogRecord& msg(std::string_view v) { if (on_) e_.message = v; return *this; }

private:
    bool on_;
    LogEvent e_;
};

namespace log {
void trace(std::string_view component, std::string_view message);
void debug(std::string_view component, std::string_view message);
void info(std::string_view component, std::string_view message);
void warn(std::string_view component, std::string_view message);
void error(std::string_view component, std::string_view message);
void critical(std::string_view component, std::string_view message);
}  // namespace log

// Reads JSON-lines log files (including rotated ones) and returns matching events.
std::vector<LogEvent> search_log_files(const std::vector<std::filesystem::path>& files, const LogQuery& q);

}  // namespace pf
