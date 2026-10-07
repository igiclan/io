#include "Logging/Logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>

#include "Common/Platform.hpp"

namespace pf {

std::string_view to_string(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error: return "error";
        case LogLevel::Critical: return "critical";
        case LogLevel::Off: return "off";
    }
    return "info";
}

bool parse_log_level(std::string_view s, LogLevel& out) noexcept {
    std::string lower;
    for (char c : s) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "warn") lower = "warning";
    for (int i = 0; i <= static_cast<int>(LogLevel::Off); ++i) {
        if (to_string(static_cast<LogLevel>(i)) == lower) {
            out = static_cast<LogLevel>(i);
            return true;
        }
    }
    return false;
}

nlohmann::json to_json(const LogEvent& e) {
    nlohmann::json j;
    j["ts"] = iso8601_utc(e.timestamp_ms);
    j["t"] = e.timestamp_ms;
    j["level"] = to_string(e.level);
    j["component"] = e.component;
    if (!e.client.empty()) j["client"] = e.client;
    if (!e.device.empty()) j["device"] = e.device;
    if (!e.session.empty()) j["session"] = e.session;
    if (!e.operation.empty()) j["op"] = e.operation;
    if (e.duration_us >= 0) j["duration_us"] = e.duration_us;
    if (!e.result.empty()) j["result"] = e.result;
    if (e.error_code != 0) j["error"] = e.error_code;
    j["msg"] = e.message;
    j["tid"] = e.thread_id;
    return j;
}

std::optional<LogEvent> log_event_from_json(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("t") || !j.contains("level")) return std::nullopt;
    LogEvent e;
    try {
        e.timestamp_ms = j.at("t").get<std::int64_t>();
        if (!parse_log_level(j.at("level").get<std::string>(), e.level)) return std::nullopt;
        e.component = j.value("component", "");
        e.client = j.value("client", "");
        e.device = j.value("device", "");
        e.session = j.value("session", "");
        e.operation = j.value("op", "");
        e.duration_us = j.value("duration_us", std::int64_t{-1});
        e.result = j.value("result", "");
        e.error_code = j.value("error", 0);
        e.message = j.value("msg", "");
        e.thread_id = j.value("tid", std::uint64_t{0});
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    return e;
}

std::string to_json_line(const LogEvent& e) {
    return to_json(e).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::optional<LogEvent> parse_json_line(std::string_view line) {
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return log_event_from_json(j);
}

std::string format_human(const LogEvent& e) {
    std::string s = iso8601_utc(e.timestamp_ms);
    s += ' ';
    std::string lvl{to_string(e.level)};
    for (auto& c : lvl) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    s += lvl;
    s += " [" + e.component + "]";
    if (!e.session.empty()) s += " session=" + e.session.substr(0, 8);
    if (!e.device.empty()) s += " device=" + e.device;
    if (!e.client.empty()) s += " client=" + e.client;
    if (!e.operation.empty()) s += " op=" + e.operation;
    if (!e.result.empty()) s += " result=" + e.result;
    if (e.duration_us >= 0) s += " dur=" + std::to_string(e.duration_us) + "us";
    if (e.error_code) s += " err=" + std::to_string(e.error_code);
    s += " | " + e.message;
    return s;
}

static bool icontains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != hay.end();
}

static bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && icontains(a, b);
}

bool LogQuery::matches(const LogEvent& e) const {
    if (e.level < min_level) return false;
    if (!component.empty() && !iequals(e.component, component)) return false;
    if (!session.empty() && e.session.compare(0, session.size(), session) != 0) return false;
    if (!device.empty() && e.device != device) return false;
    if (!client.empty() && !iequals(e.client, client)) return false;
    if (since_ms && e.timestamp_ms < since_ms) return false;
    if (until_ms && e.timestamp_ms > until_ms) return false;
    if (!text.empty() && !icontains(e.message, text) && !icontains(e.operation, text) &&
        !icontains(e.result, text))
        return false;
    return true;
}

// ------------------------------------------------------------------ sinks

void ConsoleSink::write(const LogEvent& e) {
    auto line = format_human(e);
    std::FILE* out = e.level >= LogLevel::Warning ? stderr : stdout;
    std::fprintf(out, "%s\n", line.c_str());
}

RotatingFileSink::RotatingFileSink(std::filesystem::path file, std::uint64_t max_bytes, int max_files)
    : path_(std::move(file)), max_bytes_(max_bytes), max_files_(max_files < 1 ? 1 : max_files) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    f_ = std::fopen(path_.string().c_str(), "ab");
    if (f_) size_ = std::filesystem::file_size(path_, ec);
}

RotatingFileSink::~RotatingFileSink() {
    if (f_) std::fclose(f_);
}

void RotatingFileSink::rotate() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    auto numbered = [&](int i) {
        fs::path p = path_;
        p.replace_extension(std::to_string(i) + path_.extension().string());
        return p;
    };
    fs::remove(numbered(max_files_), ec);
    for (int i = max_files_ - 1; i >= 1; --i) fs::rename(numbered(i), numbered(i + 1), ec);
    fs::rename(path_, numbered(1), ec);
    f_ = std::fopen(path_.string().c_str(), "wb");
    size_ = 0;
}

void RotatingFileSink::write(const LogEvent& e) {
    if (!f_) return;
    std::string line = to_json_line(e);
    line += '\n';
    if (size_ + line.size() > max_bytes_ && size_ > 0) rotate();
    if (!f_) return;
    std::fwrite(line.data(), 1, line.size(), f_);
    size_ += line.size();
}

void RotatingFileSink::flush() {
    if (f_) std::fflush(f_);
}

void RingBufferSink::write(const LogEvent& e) {
    std::lock_guard lk(m_);
    events_.push_back(e);
    while (events_.size() > capacity_) events_.pop_front();
}

std::vector<LogEvent> RingBufferSink::query(const LogQuery& q) const {
    std::vector<LogEvent> out;
    std::lock_guard lk(m_);
    for (auto it = events_.rbegin(); it != events_.rend() && out.size() < q.limit; ++it) {
        if (q.matches(*it)) out.push_back(*it);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::size_t RingBufferSink::size() const {
    std::lock_guard lk(m_);
    return events_.size();
}

// ------------------------------------------------------------------ logger

Logger& Logger::instance() {
    static Logger* logger = new Logger();  // leaked: usable during static destruction
    return *logger;
}

Logger::Logger() : worker_([this] { run(); }) {}

Logger::~Logger() { shutdown(); }

void Logger::add_sink(std::shared_ptr<LogSink> sink) {
    std::lock_guard lk(m_);
    sinks_.push_back(std::move(sink));
}

void Logger::clear_sinks() {
    flush();
    std::lock_guard lk(m_);
    sinks_.clear();
}

void Logger::log(LogEvent e) {
    if (!enabled(e.level)) return;
    if (e.timestamp_ms == 0) e.timestamp_ms = unix_time_ms();
    if (e.thread_id == 0) e.thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFFF;
    {
        std::lock_guard lk(m_);
        if (stop_) return;
        if (queue_.size() >= queue_limit_) {
            ++dropped_;
            return;
        }
        queue_.push_back(std::move(e));
    }
    cv_.notify_one();
}

void Logger::run() {
    set_current_thread_name("pf-logger");
    std::unique_lock lk(m_);
    for (;;) {
        cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
        if (queue_.empty() && stop_) break;
        std::deque<LogEvent> batch;
        batch.swap(queue_);
        auto sinks = sinks_;
        writing_ = true;
        lk.unlock();
        for (const auto& e : batch) {
            for (const auto& s : sinks) s->write(e);
        }
        for (const auto& s : sinks) s->flush();
        lk.lock();
        written_ += batch.size();
        writing_ = false;
        flushed_cv_.notify_all();
    }
    writing_ = false;
    flushed_cv_.notify_all();
}

void Logger::flush() {
    std::unique_lock lk(m_);
    flushed_cv_.wait(lk, [&] { return stop_ || (queue_.empty() && !writing_); });
}

void Logger::shutdown() {
    {
        std::lock_guard lk(m_);
        if (stop_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

LoggerStats Logger::stats() const {
    std::lock_guard lk(m_);
    return LoggerStats{written_, dropped_};
}

LogRecord::LogRecord(LogLevel level, std::string_view component) : on_(Logger::instance().enabled(level)) {
    e_.level = level;
    if (on_) e_.component = component;
}

LogRecord::~LogRecord() {
    if (on_) Logger::instance().log(std::move(e_));
}

namespace log {
static void emit(LogLevel l, std::string_view c, std::string_view m) {
    if (!Logger::instance().enabled(l)) return;
    LogEvent e;
    e.level = l;
    e.component = c;
    e.message = m;
    Logger::instance().log(std::move(e));
}
void trace(std::string_view c, std::string_view m) { emit(LogLevel::Trace, c, m); }
void debug(std::string_view c, std::string_view m) { emit(LogLevel::Debug, c, m); }
void info(std::string_view c, std::string_view m) { emit(LogLevel::Info, c, m); }
void warn(std::string_view c, std::string_view m) { emit(LogLevel::Warning, c, m); }
void error(std::string_view c, std::string_view m) { emit(LogLevel::Error, c, m); }
void critical(std::string_view c, std::string_view m) { emit(LogLevel::Critical, c, m); }
}  // namespace log

std::vector<LogEvent> search_log_files(const std::vector<std::filesystem::path>& files, const LogQuery& q) {
    std::vector<LogEvent> out;
    for (const auto& f : files) {
        std::ifstream in(f);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            auto e = parse_json_line(line);
            if (e && q.matches(*e)) out.push_back(std::move(*e));
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const LogEvent& a, const LogEvent& b) { return a.timestamp_ms < b.timestamp_ms; });
    if (out.size() > q.limit) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(q.limit));
    return out;
}

}  // namespace pf
