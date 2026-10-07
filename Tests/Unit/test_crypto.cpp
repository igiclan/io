#include <doctest/doctest.h>
#include <fstream>

#include "../Common/TestEnv.hpp"
#include "Crypto/Crypto.hpp"
#include "Crypto/SecretStore.hpp"
#include "Crypto/TokenStore.hpp"

using namespace pf;

TEST_CASE("sha256 / hmac known answers") {
    std::string abc = "abc";
    auto d = crypto::sha256(ByteSpan(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()));
    CHECK(to_hex(d) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // RFC 4231 test case 2
    std::string key = "Jefe", data = "what do ya want for nothing?";
    auto h = crypto::hmac_sha256(ByteSpan(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()),
                                 ByteSpan(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
    CHECK(to_hex(h) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // RFC 7914 / PBKDF2-HMAC-SHA256 vector: P="passwd", S="salt", c=1, dkLen=64
    std::string salt = "salt";
    auto k = crypto::pbkdf2_sha256("passwd", ByteSpan(reinterpret_cast<const std::uint8_t*>(salt.data()), 4), 1, 64);
    CHECK(to_hex(k).substr(0, 32) == "55ac046e56e3089fec1691c22544b605");
}

TEST_CASE("random and uuid") {
    CHECK(crypto::random_hex(16).size() == 32);
    CHECK(crypto::random_hex(16) != crypto::random_hex(16));
    auto u = crypto::uuid4();
    CHECK(u.size() == 36);
    CHECK(u[14] == '4');
    CHECK(crypto::constant_time_equal(std::string_view("abc"), std::string_view("abc")));
    CHECK_FALSE(crypto::constant_time_equal(std::string_view("abc"), std::string_view("abd")));
}

TEST_CASE("token store never persists plaintext and verifies tokens") {
    test::TempDir dir;
    TokenStore store(dir / "tokens.json");
    REQUIRE(store.load().ok());
    auto tok = store.create("bench", false, {"18d1:*"});
    REQUIRE(tok.ok());
    CHECK(tok->rfind("pf_", 0) == 0);
    std::string content;
    {
        std::ifstream in(dir / "tokens.json");  // must be closed before the store rewrites it (Windows)
        content.assign((std::istreambuf_iterator<char>(in)), {});
    }
    CHECK(content.find(tok->substr(12)) == std::string::npos);  // secret not on disk

    TokenStore reloaded(dir / "tokens.json");
    REQUIRE(reloaded.load().ok());
    auto id = reloaded.verify(*tok);
    REQUIRE(id.ok());
    CHECK(id->name == "bench");
    CHECK(id->allowed_devices.size() == 1);
    std::string forged = *tok;
    forged.back() = forged.back() == '0' ? '1' : '0';
    CHECK_FALSE(reloaded.verify(forged).ok());
    CHECK_FALSE(reloaded.verify("garbage").ok());
    REQUIRE(reloaded.revoke(id->id).ok());
    CHECK_FALSE(reloaded.verify(*tok).ok());
}

TEST_CASE("glob matching") {
    CHECK(TokenStore::glob_match("18d1:*", "18d1:4ee7"));
    CHECK(TokenStore::glob_match("*", "anything"));
    CHECK(TokenStore::glob_match("05c6:900?", "05c6:9008"));
    CHECK_FALSE(TokenStore::glob_match("18d1:*", "05c6:9008"));
}

TEST_CASE("self-signed certificate generation and fingerprint") {
    test::TempDir dir;
    crypto::CertificateFiles files{dir / "c.crt", dir / "c.key"};
    REQUIRE(crypto::ensure_self_signed_certificate(files, "unit").ok());
    auto fp1 = crypto::certificate_fingerprint_file(files.cert_pem);
    REQUIRE(fp1.ok());
    CHECK(fp1->size() == 95);  // 32 bytes as aa:bb:...
    REQUIRE(crypto::ensure_self_signed_certificate(files, "unit").ok());  // idempotent
    CHECK(*crypto::certificate_fingerprint_file(files.cert_pem) == *fp1);
    CHECK(crypto::normalize_fingerprint("AA:bb") == "aabb");
}

TEST_CASE("secret store round trip") {
    test::TempDir dir;
    {
        SecretStore s(dir / "secrets.json");
        REQUIRE(s.load().ok());
        REQUIRE(s.put("server1", "pf_token_value").ok());
    }
    SecretStore s2(dir / "secrets.json");
    REQUIRE(s2.load().ok());
    auto v = s2.get("server1");
    REQUIRE(v.ok());
    CHECK(*v == "pf_token_value");
    CHECK_FALSE(s2.get("missing").ok());
}
