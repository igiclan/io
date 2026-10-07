#include <doctest/doctest.h>

#include <fstream>
#include <nlohmann/json.hpp>

#include "../Common/TestEnv.hpp"
#include "Common/Config.hpp"

using namespace pf;

TEST_CASE("server config defaults are valid and created on first load") {
    test::TempDir dir;
    auto cfg = load_server_config(dir / "config.json");
    REQUIRE(cfg.ok());
    CHECK(cfg->server_port == 39000);
    CHECK(cfg->tls_enabled);
    CHECK(cfg->reconnect_timeout_ms == 60000);
    CHECK(cfg->mobile_repair_mode);
    CHECK_FALSE(cfg->cert_file.empty());
    CHECK(std::filesystem::exists(dir / "config.json"));
}

TEST_CASE("server config parse + validation") {
    ServerConfig c;
    auto j = nlohmann::json::parse(R"({"server_port": 40000, "tls_enabled": false, "max_clients": 4,
        "keepalive": {"interval_ms": 1000}, "performance_mode": "low-latency", "buffer_size": 131072,
        "usb": {"backend": "test", "test_devices": 2}})");
    REQUIRE(from_json(j, c).ok());
    CHECK(c.server_port == 40000);
    CHECK_FALSE(c.tls_enabled);
    CHECK(c.performance_mode == PerformanceMode::LowLatency);
    CHECK(c.network.read_buffer_size == 131072);
    CHECK(c.usb.test_devices == 2);
    ServerConfig bad;
    CHECK_FALSE(from_json(nlohmann::json::parse(R"({"server_port": 0})"), bad).ok());
    CHECK_FALSE(from_json(nlohmann::json::parse(R"({"performance_mode": "turbo"})"), bad).ok());
    CHECK_FALSE(from_json(nlohmann::json::parse(R"({"server_port": "x"})"), bad).ok());
    CHECK_FALSE(from_json(nlohmann::json::parse(R"({"log_level": "loud"})"), bad).ok());
}

TEST_CASE("client config round trip") {
    ClientConfig c;
    c.client_id = "id-1";
    ServerEntry s;
    s.name = "Bench";
    s.host = "10.0.0.5";
    s.fingerprint = "aa:bb";
    c.servers.push_back(s);
    ClientConfig back;
    REQUIRE(from_json(to_json(c), back).ok());
    REQUIRE(back.servers.size() == 1);
    CHECK(back.servers[0].host == "10.0.0.5");
    CHECK(back.servers[0].fingerprint == "aa:bb");
    CHECK(back.client_id == "id-1");
}
