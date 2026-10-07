#include <doctest/doctest.h>

#include "../Common/TestEnv.hpp"
#include "Logging/Logger.hpp"

using namespace pf;

TEST_CASE("log event JSON round trip") {
    LogEvent e;
    e.timestamp_ms = 1700000000123;
    e.level = LogLevel::Warning;
    e.component = "USB";
    e.client = "PC-01";
    e.device = "d1";
    e.session = "abcdef0123456789";
    e.operation = "attach";
    e.duration_us = 1500;
    e.result = "ok";
    e.error_code = 401;
    e.message = "پیام \"quoted\"";
    auto back = parse_json_line(to_json_line(e));
    REQUIRE(back);
    CHECK(back->level == LogLevel::Warning);
    CHECK(back->message == e.message);
    CHECK(back->duration_us == 1500);
    CHECK(back->error_code == 401);
    CHECK_FALSE(parse_json_line("{broken"));
}

TEST_CASE("log query filters") {
    LogEvent e;
    e.level = LogLevel::Info;
    e.component = "Network";
    e.session = "abcdef";
    e.message = "Connection established";
    LogQuery q;
    CHECK(q.matches(e));
    q.min_level = LogLevel::Error;
    CHECK_FALSE(q.matches(e));
    q = {};
    q.component = "network";
    q.text = "ESTABLISHED";
    q.session = "abc";
    CHECK(q.matches(e));
    q.session = "zzz";
    CHECK_FALSE(q.matches(e));
}

TEST_CASE("async logger writes to ring and rotating file; files are searchable") {
    test::TempDir dir;
    auto ring = std::make_shared<RingBufferSink>(100);
    auto file = std::make_shared<RotatingFileSink>(dir / "test.log", 2000, 3);
    auto& lg = Logger::instance();
    lg.set_level(LogLevel::Debug);
    lg.add_sink(ring);
    lg.add_sink(file);
    for (int i = 0; i < 100; ++i) {
        LogRecord(LogLevel::Info, "Test").session("s" + std::to_string(i % 3)).op("op").msg("message " + std::to_string(i));
    }
    LogRecord(LogLevel::Trace, "Test").msg("filtered out by level");
    lg.flush();
    lg.clear_sinks();
    CHECK(ring->size() == 100);
    LogQuery q;
    q.session = "s1";
    CHECK(ring->query(q).size() == 33);
    CHECK(std::filesystem::exists(dir / "test.1.log"));  // rotated
    std::vector<std::filesystem::path> files;
    for (auto& p : std::filesystem::directory_iterator(dir.path())) files.push_back(p.path());
    LogQuery fq;
    fq.text = "message 99";
    auto found = search_log_files(files, fq);
    REQUIRE(found.size() == 1);
    CHECK(found[0].session == "s0");
}
