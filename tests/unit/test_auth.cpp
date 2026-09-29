#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "include/auth/authenticator.h"
#include "include/auth/authorize.h"
#include "include/auth/crypto.h"
#include "include/auth/ip.h"
#include "include/auth/secret_backend.h"
#include "include/auth/token_cli.h"
#include "include/auth/token_store.h"
#include "include/command_table.h"
#include "include/mcp/tool_catalog.h"

using namespace fairyfly;
using namespace fairyfly::auth;
using fairyfly::mcp::AuthOutcome;
using fairyfly::mcp::AuthRequest;

namespace {

struct FakeClock {
    TimePoint now = std::chrono::sys_seconds(std::chrono::seconds(1'800'000'000));  // 2027-01-15
    Clock fn() { return [this] { return now; }; }
    void advance(std::chrono::seconds s) { now += s; }
};

/// Deterministic, never-repeating byte source (tests must not depend on real randomness).
RandomFn counting_rng() {
    auto counter = std::make_shared<unsigned>(1);
    return [counter](unsigned char* buf, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) buf[i] = static_cast<unsigned char>((*counter * 37 + i * 11 + 5) & 0xff);
        ++*counter;
    };
}

struct Env {
    std::shared_ptr<InMemorySecretBackend> tokens = std::make_shared<InMemorySecretBackend>();
    std::shared_ptr<InMemorySecretBackend> proxy = std::make_shared<InMemorySecretBackend>();
    FakeClock clock;
    RandomFn rng = counting_rng();
    AuthConfig config;
    std::unique_ptr<TokenAuthenticator> auth;

    explicit Env(const std::string& proxy_secret = "", bool with_proxy_in_backend = false) {
        config.token_backend = tokens;
        config.proxy_backend = proxy;
        config.clock = clock.fn();
        config.cache_ttl = std::chrono::milliseconds(0);  // every request sees the store as it is now
        config.proxy_secret = with_proxy_in_backend ? "" : proxy_secret;
        if (with_proxy_in_backend) proxy->put(config.proxy_name, proxy_secret);
        auth = std::make_unique<TokenAuthenticator>(config);
    }
    CreatedToken create(const std::string& name, NewToken t = {}) {
        t.name = name;
        if (t.scopes.empty()) t.scopes = {"screen"};
        TokenStore store(tokens, clock.fn(), rng, std::chrono::milliseconds(0));
        return store.create(t);
    }
    TokenStore store() { return TokenStore(tokens, clock.fn(), rng, std::chrono::milliseconds(0)); }
};

AuthRequest request_with(const std::string& token, const std::string& peer = "127.0.0.1") {
    AuthRequest r;
    r.authorization = token.empty() ? "" : "Bearer " + token;
    r.peer_addr = peer;
    return r;
}

std::string secret_of(const std::string& token) { return token.substr(13); }

} // namespace

// ---------------------------------------------------------------------------------------------
TEST_CASE("auth: sha256 and constant-time compare", "[auth]") {
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(constant_time_equal("abc", "abc"));
    CHECK_FALSE(constant_time_equal("abc", "abd"));
    CHECK_FALSE(constant_time_equal("abc", "abcd"));
    CHECK_FALSE(constant_time_equal("", "a"));
    CHECK(constant_time_equal("", ""));
    CHECK(base64url_encode(reinterpret_cast<const unsigned char*>("\xfb\xff\xfe"), 3) == "-__-");
}

TEST_CASE("auth: real CSPRNG yields distinct 32 byte secrets", "[auth]") {
    unsigned char a[32] = {}, b[32] = {};
    random_bytes(a, 32);
    random_bytes(b, 32);
    CHECK(std::memcmp(a, b, 32) != 0);
}

TEST_CASE("auth: token format ffy_<id>_<secret>", "[auth][token]") {
    Env env;
    const auto created = env.create("ci-bot");
    const std::string& token = created.token;
    REQUIRE(token.size() == 4 + 8 + 1 + 43);
    CHECK(token.rfind("ffy_", 0) == 0);
    const auto parsed = parse_token(token);
    REQUIRE(parsed);
    CHECK(parsed->id == created.meta.id);
    CHECK(parsed->id.size() == 8);
    CHECK(parsed->secret.size() == 43);
    CHECK(created.meta.secret_hash == sha256_hex(parsed->secret));
    CHECK(token_display_id(created.meta) == "ffy_" + created.meta.id + "_...");

    CHECK_FALSE(parse_token(""));
    CHECK_FALSE(parse_token("ffy_12345678_short"));
    CHECK_FALSE(parse_token("xxx_" + token.substr(4)));
    CHECK_FALSE(parse_token("ffy_ZZZZZZZZ_" + token.substr(13)));       // id must be lower-case hex
    CHECK_FALSE(parse_token(token + "x"));
    CHECK_FALSE(parse_token(token.substr(0, 12) + "-" + token.substr(13)));  // separator must be '_'
}

TEST_CASE("auth: the secret is never stored, only its hash", "[auth][token]") {
    Env env;
    const auto created = env.create("ci-bot");
    const std::string secret = secret_of(created.token);
    REQUIRE(env.tokens->size() == 1);
    for (const auto& value : env.tokens->all_values()) {
        CHECK(value.find(secret) == std::string::npos);
        CHECK(value.find(created.token) == std::string::npos);
        CHECK(value.find(created.meta.secret_hash) != std::string::npos);
    }
    const auto blob = nlohmann::json::parse(env.tokens->all_values().front());
    for (const char* key : {"v", "i", "n", "h", "c", "s", "o"}) CHECK(blob.contains(key));  // compact keys
    CHECK_FALSE(blob.contains("e"));  // optional
    CHECK_FALSE(blob.contains("t"));  // empty lists are omitted
}

namespace {
NewToken huge_token(std::size_t systems) {
    NewToken t;
    t.scopes = {"screen"};
    for (std::size_t i = 0; i < systems; ++i) t.sap_systems.push_back("S" + std::to_string(1000 + i) + "/" + std::to_string(100 + i % 800));
    return t;
}
} // namespace

TEST_CASE("auth: a very long allowlist is chunked and read back whole", "[auth][token][chunks]") {
    Env env;
    const auto t = huge_token(200);  // ~2 KB compact
    const auto created = env.create("big", t);
    CHECK(env.tokens->size() > 1);   // head + chunks
    auto names = env.tokens->list_names();
    CHECK(std::count(names.begin(), names.end(), "big#1") == 1);
    for (const auto& value : env.tokens->all_values()) CHECK(value.size() <= 1200);
    // the token still authenticates and carries the full allowlist
    const auto listed = env.store().list();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].sap_systems == t.sap_systems);
    CHECK(env.store().find_by_id(created.meta.id).has_value());
    const auto outcome = env.auth->authenticate(request_with(created.token));
    REQUIRE(outcome.ok);
    CHECK(outcome.principal.sap_systems.size() == 200);
    // revoke and rotate keep working on a chunked record
    auto store = env.store();
    const auto rotated = store.rotate("big");
    CHECK(store.list()[0].sap_systems == t.sap_systems);
    CHECK(env.auth->authenticate(request_with(created.token)).ok == false);
    CHECK(env.auth->authenticate(request_with(rotated.token)).ok);
    CHECK(store.revoke("big"));
    CHECK(store.list()[0].revoked);
    CHECK(store.list()[0].sap_systems == t.sap_systems);
}

TEST_CASE("auth: too large even for chunks is refused as TOKEN_TOO_LARGE", "[auth][token][chunks]") {
    Env env;
    auto store = env.store();
    NewToken t = huge_token(2500);
    t.name = "toobig";
    try {
        store.create(t);
        FAIL("expected TOKEN_TOO_LARGE");
    } catch (const AuthError& e) {
        CHECK(e.code() == "TOKEN_TOO_LARGE");
    }
    CHECK(store.list().empty());
}

TEST_CASE("auth: a missing chunk or a missing head never authenticates", "[auth][token][chunks]") {
    Env env;
    const auto created = env.create("big", huge_token(200));
    REQUIRE(env.store().find_by_id(created.meta.id));
    env.tokens->remove("big#2");  // interrupted or damaged set
    CHECK(env.store().list().empty());
    CHECK_FALSE(env.store().find_by_id(created.meta.id).has_value());
    CHECK_FALSE(env.auth->authenticate(request_with(created.token)).ok);
    auto store = env.store();
    CHECK_THROWS_AS(store.revoke("big"), AuthError);
    CHECK_THROWS_AS(store.rotate("big"), AuthError);
    // delete cleans the remains up and frees the name
    CHECK(store.remove("big"));
    CHECK(env.tokens->size() == 0);
    CHECK_NOTHROW(env.create("big", huge_token(200)));
}

TEST_CASE("auth: a partial write (chunks without head) is invisible and cleaned by delete", "[auth][token][chunks]") {
    Env env;
    env.tokens->put("half#1", "{\"v\":2,\"i\":\"deadbeef\"");
    env.tokens->put("half#2", "garbage");
    CHECK(env.store().list().empty());
    CHECK(env.store().count() == 0);
    auto store = env.store();
    CHECK_FALSE(store.remove("half"));  // no such token, but the orphans are swept
    CHECK(env.tokens->size() == 0);
    // a corrupted chunk (hash mismatch) is rejected as well
    const auto created = env.create("big", huge_token(200));
    env.tokens->put("big#1", std::string(1000, 'x'));
    CHECK_FALSE(env.auth->authenticate(request_with(created.token)).ok);
}

TEST_CASE("auth: shrinking a chunked token drops its stale chunks; delete removes every chunk", "[auth][token][chunks]") {
    Env env;
    env.create("big", huge_token(300));
    const std::size_t before = env.tokens->size();
    CHECK(before >= 4);
    auto store = env.store();
    // rewrite the same name with a smaller record (as a rewrite of the entry would)
    NewToken small = huge_token(3);
    small.name = "big";
    TokenMeta meta = store.list()[0];
    meta.sap_systems = small.sap_systems;
    const std::string blob = meta.to_compact_json().dump();
    env.tokens->put("big", blob);  // old-style single write over a chunked head
    CHECK(store.list()[0].sap_systems == small.sap_systems);
    auto rotated = store.rotate("big");  // save() removes the orphans
    (void)rotated;
    CHECK(env.tokens->size() == 1);
    CHECK(store.remove("big"));
    CHECK(env.tokens->size() == 0);
}

TEST_CASE("auth: legacy long-key single-entry records still load, revoke and rotate", "[auth][token][chunks]") {
    Env env;
    const auto created = env.create("legacy");
    TokenMeta meta = env.store().list()[0];
    env.tokens->put("legacy", meta.to_stored_json().dump());  // the pre-compact format
    auto store = env.store();
    REQUIRE(store.list().size() == 1);
    CHECK(store.list()[0].secret_hash == created.meta.secret_hash);
    CHECK(env.auth->authenticate(request_with(created.token)).ok);
    CHECK(store.revoke("legacy"));
    CHECK(store.list()[0].revoked);
    const auto again = env.create("legacy2");
    env.tokens->put("legacy2", env.store().list()[1].to_stored_json().dump());
    CHECK_NOTHROW(store.rotate("legacy2"));
    (void)again;
}

TEST_CASE("auth: create validates names, scopes and restrictions", "[auth][token]") {
    Env env;
    auto store = env.store();
    NewToken t;
    t.scopes = {"screen"};

    t.name = "bad name!";
    CHECK_THROWS_AS(store.create(t), AuthError);
    t.name = "";
    CHECK_THROWS_AS(store.create(t), AuthError);

    t.name = "ok";
    t.scopes = {"screen", "bogus"};
    try {
        store.create(t);
        FAIL("expected UNKNOWN_FAMILY");
    } catch (const AuthError& e) {
        CHECK(e.code() == "UNKNOWN_FAMILY");
    }
    t.scopes = {};
    CHECK_THROWS_AS(store.create(t), AuthError);

    t.scopes = {"screen"};
    t.allowed_ips = {"not-an-ip"};
    try {
        store.create(t);
        FAIL("expected INVALID_IP");
    } catch (const AuthError& e) {
        CHECK(e.code() == "INVALID_IP");
    }
    t.allowed_ips = {};
    t.sap_systems = {"A4H 001"};
    CHECK_THROWS_AS(store.create(t), AuthError);
    t.sap_systems = {"A4H/001"};
    CHECK_NOTHROW(store.create(t));
    try {
        store.create(t);
        FAIL("expected TOKEN_EXISTS");
    } catch (const AuthError& e) {
        CHECK(e.code() == "TOKEN_EXISTS");
    }
    // every family of the command table plus "*" is a valid scope
    NewToken all;
    all.name = "allfam";
    all.scopes = command_table::families();
    all.scopes.push_back("*");
    CHECK_NOTHROW(store.create(all));
}

TEST_CASE("auth: create, list, revoke, rotate", "[auth][token]") {
    Env env;
    NewToken t;
    t.scopes = {"screen", "element"};
    t.sap_systems = {"A4H/001"};
    t.tcodes = {"SE16", "SM*"};
    t.rate_per_minute = 30;
    t.allowed_ips = {"10.0.0.0/8"};
    t.read_only = false;
    t.expires = env.clock.now + std::chrono::hours(24);
    const auto first = env.create("alpha", t);
    const auto second = env.create("beta");

    auto store = env.store();
    const auto listed = store.list();
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].name == "alpha");
    CHECK(listed[0].scopes == std::vector<std::string>{"screen", "element"});
    CHECK(listed[0].sap_systems == std::vector<std::string>{"A4H/001"});
    CHECK(listed[0].tcodes == std::vector<std::string>{"SE16", "SM*"});
    CHECK(listed[0].rate_per_minute == 30);
    CHECK(listed[0].allowed_ips == std::vector<std::string>{"10.0.0.0/8"});
    CHECK_FALSE(listed[0].read_only);
    REQUIRE(listed[0].expires);
    CHECK(*listed[0].expires == *t.expires);
    CHECK(listed[1].read_only);  // TokenMeta default when NewToken keeps the default

    // public listing never carries the hash
    const auto pub = listed[0].to_public_json();
    CHECK_FALSE(pub.contains("sha256"));
    CHECK(pub.dump().find(first.meta.secret_hash) == std::string::npos);

    CHECK(store.revoke("alpha"));
    CHECK_FALSE(store.revoke("nope"));
    CHECK(store.list()[0].revoked);

    const auto rotated = store.rotate("beta");
    CHECK(rotated.meta.name == "beta");
    CHECK(rotated.meta.id != second.meta.id);
    CHECK(rotated.token != second.token);
    CHECK(rotated.meta.scopes == second.meta.scopes);
    CHECK(rotated.meta.read_only == second.meta.read_only);
    CHECK_THROWS_AS(store.rotate("alpha"), AuthError);  // revoked
    CHECK_THROWS_AS(store.rotate("missing"), AuthError);
}

TEST_CASE("auth: expiry syntax", "[auth][token]") {
    const TimePoint now = std::chrono::sys_seconds(std::chrono::seconds(1'800'000'000));
    CHECK(*parse_expiry("30d", now) == now + std::chrono::hours(24 * 30));
    CHECK(*parse_expiry("12h", now) == now + std::chrono::hours(12));
    CHECK(*parse_expiry("90m", now) == now + std::chrono::minutes(90));
    const auto date = parse_expiry("2026-12-31", now);
    REQUIRE(date);
    CHECK(format_iso_utc(*date) == "2026-12-31T00:00:00Z");
    CHECK(format_iso_utc(*parse_iso_utc("2027-01-15T08:00:00Z")) == "2027-01-15T08:00:00Z");
    CHECK_FALSE(parse_expiry("soon", now));
    CHECK_FALSE(parse_expiry("0d", now));
    CHECK_FALSE(parse_expiry("2026-13-40", now));
    CHECK_FALSE(parse_expiry("", now));
}

TEST_CASE("auth: token delete removes revoked and active tokens for good", "[auth][token]") {
    Env env;
    const auto keep = env.create("keep");
    const auto old = env.create("old");
    auto store = env.store();
    REQUIRE(store.revoke("old"));
    CHECK(store.find_by_id(old.meta.id)->revoked);

    TokenCliArgs args;
    args.action = "delete";
    args.name = "old";
    // needs --yes
    auto r = run_token_action(args, store);
    CHECK(r.status == Result::Status::Error);
    CHECK(r.error["code"] == "CONFIRMATION_REQUIRED");
    CHECK(store.list().size() == 2);

    args.yes = true;
    r = run_token_action(args, store);  // a REVOKED token can be deleted
    CHECK(r.status == Result::Status::Success);
    CHECK(r.data["deleted"] == true);
    const auto left = store.list();
    REQUIRE(left.size() == 1);
    CHECK(left[0].name == "keep");
    CHECK_FALSE(store.find_by_id(old.meta.id).has_value());
    CHECK(store.find_by_id(keep.meta.id).has_value());
    // the name is free again
    CHECK_NOTHROW(env.create("old"));

    // unknown name
    args.name = "missing";
    r = run_token_action(args, store);
    CHECK(r.error["code"] == "TOKEN_NOT_FOUND");

    // an active token can be deleted too
    args.name = "keep";
    CHECK(run_token_action(args, store).status == Result::Status::Success);
    CHECK_FALSE(store.find_by_id(keep.meta.id).has_value());
}

TEST_CASE("auth: revoke is visible to a second store after the cache TTL or an explicit invalidate", "[auth][token]") {
    Env env;
    const auto created = env.create("ci-bot");
    FakeClock clock;
    TokenStore cached(env.tokens, clock.fn(), counting_rng(), std::chrono::milliseconds(5000));
    REQUIRE(cached.find_by_id(created.meta.id));
    CHECK_FALSE(cached.find_by_id(created.meta.id)->revoked);

    env.store().revoke("ci-bot");  // another process
    CHECK_FALSE(cached.find_by_id(created.meta.id)->revoked);  // still cached
    clock.advance(std::chrono::seconds(6));
    CHECK(cached.find_by_id(created.meta.id)->revoked);        // TTL passed
}

// ---- authenticator ---------------------------------------------------------------------------
TEST_CASE("auth: no tokens configured answers 401 AUTH_REQUIRED with the hint", "[auth][authn]") {
    Env env;
    for (const std::string& header : {std::string(), std::string("Bearer ffy_00000000_") + std::string(43, 'a')}) {
        AuthRequest r;
        r.authorization = header;
        r.peer_addr = "127.0.0.1";
        const auto out = env.auth->authenticate(r);
        CHECK_FALSE(out.ok);
        CHECK(out.http_status == 401);
        CHECK(out.error_code == "AUTH_REQUIRED");
        CHECK(out.message.find("fairyfly mcp token create") != std::string::npos);
        CHECK(out.www_authenticate == "Bearer realm=\"fairyfly\"");
    }
}

TEST_CASE("auth: rejection matrix", "[auth][authn]") {
    Env env;
    const auto created = env.create("ci-bot");
    const std::string secret = secret_of(created.token);

    SECTION("missing header") {
        const auto out = env.auth->authenticate(request_with(""));
        CHECK(out.http_status == 401);
        CHECK(out.error_code == "AUTH_REQUIRED");
        CHECK(out.www_authenticate == "Bearer realm=\"fairyfly\"");
    }
    SECTION("malformed") {
        for (const std::string& bad : {std::string("garbage"), std::string("ffy_short"), created.token.substr(0, 20)}) {
            const auto out = env.auth->authenticate(request_with(bad));
            CHECK(out.http_status == 401);
            CHECK(out.error_code == "TOKEN_INVALID");
            CHECK(out.www_authenticate.find("Bearer realm=\"fairyfly\"") == 0);
        }
        AuthRequest basic = request_with("");
        basic.authorization = "Basic dXNlcjpwYXNz";
        CHECK(env.auth->authenticate(basic).error_code == "TOKEN_INVALID");
    }
    SECTION("unknown id and wrong secret are indistinguishable") {
        const std::string wrong_secret = "ffy_" + created.meta.id + "_" + std::string(43, 'A');
        const std::string unknown_id = "ffy_" + std::string(created.meta.id == "00000000" ? "11111111" : "00000000") + "_" + secret;
        const auto a = env.auth->authenticate(request_with(wrong_secret));
        const auto b = env.auth->authenticate(request_with(unknown_id));
        CHECK(a.http_status == b.http_status);
        CHECK(a.error_code == "TOKEN_INVALID");
        CHECK(a.error_code == b.error_code);
        CHECK(a.message == b.message);
        CHECK(a.www_authenticate == b.www_authenticate);
    }
    SECTION("scheme is case-insensitive, a valid token authenticates") {
        AuthRequest r = request_with("");
        r.authorization = "bearer  " + created.token;
        const auto out = env.auth->authenticate(r);
        REQUIRE(out.ok);
        CHECK(out.principal.name == "ci-bot");
        CHECK(out.principal.authenticated);
        CHECK(out.principal.remote_addr == "127.0.0.1");
    }
    SECTION("no message ever contains the secret") {
        for (const std::string& t : {created.token, "ffy_" + created.meta.id + "_" + std::string(43, 'B')}) {
            const auto out = env.auth->authenticate(request_with(t));
            CHECK(out.message.find(secret) == std::string::npos);
            CHECK(out.message.find(t) == std::string::npos);
        }
    }
}

TEST_CASE("auth: expired and revoked tokens", "[auth][authn]") {
    Env env;
    NewToken t;
    t.expires = env.clock.now + std::chrono::hours(1);
    const auto created = env.create("short-lived", t);
    CHECK(env.auth->authenticate(request_with(created.token)).ok);

    env.clock.advance(std::chrono::minutes(59));
    CHECK(env.auth->authenticate(request_with(created.token)).ok);
    env.clock.advance(std::chrono::minutes(2));
    const auto expired = env.auth->authenticate(request_with(created.token));
    CHECK_FALSE(expired.ok);
    CHECK(expired.http_status == 401);
    CHECK(expired.error_code == "TOKEN_EXPIRED");
    CHECK_FALSE(expired.www_authenticate.empty());

    const auto other = env.create("to-revoke");
    CHECK(env.auth->authenticate(request_with(other.token)).ok);
    env.store().revoke("to-revoke");
    const auto revoked = env.auth->authenticate(request_with(other.token));
    CHECK(revoked.http_status == 401);
    CHECK(revoked.error_code == "TOKEN_REVOKED");

    // rotate: the old secret dies at once, the new one works
    const auto rot = env.create("rot");
    const auto fresh = env.store().rotate("rot");
    CHECK(env.auth->authenticate(request_with(rot.token)).error_code == "TOKEN_INVALID");
    CHECK(env.auth->authenticate(request_with(fresh.token)).ok);
}

TEST_CASE("auth: the token cache bounds revocation latency", "[auth][authn]") {
    Env env;
    env.config.cache_ttl = std::chrono::milliseconds(5000);
    TokenAuthenticator cached(env.config);
    const auto created = env.create("ci-bot");
    CHECK(cached.authenticate(request_with(created.token)).ok);
    env.store().revoke("ci-bot");
    CHECK(cached.authenticate(request_with(created.token)).ok);  // stale within the TTL...
    cached.invalidate();                                          // ...explicit invalidate makes it instant
    CHECK(cached.authenticate(request_with(created.token)).error_code == "TOKEN_REVOKED");
}

TEST_CASE("auth: IP binding with addresses and CIDR", "[auth][authn][ip]") {
    CHECK(parse_ip("10.1.2.3"));
    CHECK(parse_ip("[::1]:8080"));
    CHECK(parse_ip("10.1.2.3:55"));
    CHECK_FALSE(parse_ip("nonsense"));
    CHECK_FALSE(parse_ip(""));
    CHECK(ip_allowed("10.1.2.3", {"10.0.0.0/8"}));
    CHECK_FALSE(ip_allowed("11.1.2.3", {"10.0.0.0/8"}));
    CHECK(ip_allowed("192.168.1.77", {"192.168.1.0/24", "1.1.1.1"}));
    CHECK_FALSE(ip_allowed("192.168.2.77", {"192.168.1.0/24"}));
    CHECK(ip_allowed("1.1.1.1", {"1.1.1.1"}));
    CHECK_FALSE(ip_allowed("1.1.1.2", {"1.1.1.1"}));
    CHECK(ip_allowed("fd00::1234", {"fd00::/8"}));
    CHECK_FALSE(ip_allowed("fe80::1", {"fd00::/8"}));
    CHECK(ip_allowed("::ffff:10.1.2.3", {"10.0.0.0/8"}));       // IPv4-mapped IPv6
    CHECK(ip_allowed("127.0.0.1", {"0.0.0.0/0"}));
    CHECK_FALSE(ip_allowed("garbage", {"0.0.0.0/0"}));           // fail closed
    CHECK_FALSE(parse_ip_rule("10.0.0.0/33"));
    CHECK_FALSE(parse_ip_rule("10.0.0.0/x"));

    Env env;
    NewToken t;
    t.allowed_ips = {"10.0.0.0/8", "203.0.113.9"};
    const auto created = env.create("bound", t);
    CHECK(env.auth->authenticate(request_with(created.token, "10.20.30.40")).ok);
    CHECK(env.auth->authenticate(request_with(created.token, "203.0.113.9")).ok);
    const auto denied = env.auth->authenticate(request_with(created.token, "198.51.100.1"));
    CHECK_FALSE(denied.ok);
    CHECK(denied.http_status == 403);
    CHECK(denied.error_code == "IP_NOT_ALLOWED");
    CHECK(denied.www_authenticate.empty());
    // an unbound token accepts any address
    const auto open = env.create("open");
    CHECK(env.auth->authenticate(request_with(open.token, "198.51.100.1")).ok);
}

TEST_CASE("auth: trusted proxy matrix", "[auth][authn][proxy]") {
    const std::string kSecret = "proxy-shared-secret";
    NewToken t;
    t.allowed_ips = {"203.0.113.0/24"};

    auto request = [&](const Env& env, const std::string& token, const std::string& xff, const std::string& proxy_header,
                       const std::string& peer = "127.0.0.1") {
        (void)env;
        AuthRequest r = request_with(token, peer);
        r.forwarded_for = xff;
        r.forwarded_proto = xff.empty() ? "" : "https";
        r.proxy_secret = proxy_header;
        return r;
    };

    SECTION("secret configured and matching: forwarded address is used") {
        Env env(kSecret);
        const auto c = env.create("bound", t);
        const auto out = env.auth->authenticate(request(env, c.token, "203.0.113.7", kSecret));
        REQUIRE(out.ok);
        CHECK(out.principal.remote_addr == "203.0.113.7");
        // the proxy's own (last) entry wins over a client-forged first entry
        const auto forged = env.auth->authenticate(request(env, c.token, "203.0.113.7, 198.51.100.1", kSecret));
        CHECK_FALSE(forged.ok);
        CHECK(forged.error_code == "IP_NOT_ALLOWED");
        const auto ok_last = env.auth->authenticate(request(env, c.token, "198.51.100.1, 203.0.113.8", kSecret));
        CHECK(ok_last.ok);
    }
    SECTION("secret configured but wrong or missing: forwarded headers are ignored, the peer address is used") {
        Env env(kSecret);
        const auto c = env.create("bound", t);
        for (const std::string& header : {std::string("wrong"), std::string()}) {
            const auto out = env.auth->authenticate(request(env, c.token, "203.0.113.7", header));
            CHECK_FALSE(out.ok);
            CHECK(out.error_code == "IP_NOT_ALLOWED");  // peer 127.0.0.1 is not in the bound range
        }
        const auto peer_ok = env.auth->authenticate(request(env, c.token, "198.51.100.1", "wrong", "203.0.113.50"));
        REQUIRE(peer_ok.ok);
        CHECK(peer_ok.principal.remote_addr == "203.0.113.50");
    }
    SECTION("no proxy secret configured: the header is never trusted") {
        Env env("");
        const auto c = env.create("bound", t);
        const auto out = env.auth->authenticate(request(env, c.token, "203.0.113.7", "anything"));
        CHECK_FALSE(out.ok);
        CHECK(out.error_code == "IP_NOT_ALLOWED");
        CHECK(env.auth->resolve_client(request(env, c.token, "203.0.113.7", "")).proxy_trusted == false);
    }
    SECTION("secret read from the backend (Credential Manager entry)") {
        Env env(kSecret, /*with_proxy_in_backend=*/true);
        const auto c = env.create("bound", t);
        const auto out = env.auth->authenticate(request(env, c.token, "203.0.113.7", kSecret));
        REQUIRE(out.ok);
        CHECK(out.principal.remote_addr == "203.0.113.7");
        CHECK(env.auth->resolve_client(request(env, c.token, "203.0.113.7", kSecret)).proxy_trusted);
    }
    SECTION("forwarded address without port and with port") {
        Env env(kSecret);
        const auto c = env.create("open");
        CHECK(env.auth->authenticate(request(env, c.token, "203.0.113.7:4711", kSecret)).principal.remote_addr == "203.0.113.7:4711");
    }
}

TEST_CASE("auth: principal carries the token restrictions", "[auth][authn]") {
    Env env;
    NewToken t;
    t.scopes = {"screen", "element"};
    t.sap_systems = {"A4H/001"};
    t.tcodes = {"SE16"};
    t.rate_per_minute = 12;
    t.read_only = true;
    const auto c = env.create("ci", t);
    const auto out = env.auth->authenticate(request_with(c.token, "10.0.0.9"));
    REQUIRE(out.ok);
    const auto& p = out.principal;
    CHECK(p.name == "ci");
    CHECK_FALSE(p.all_scopes);
    CHECK(p.scopes == std::set<std::string>{"screen", "element"});
    CHECK(p.sap_systems == std::vector<std::string>{"A4H/001"});
    CHECK(p.tcodes == std::vector<std::string>{"SE16"});
    CHECK(p.rate_per_minute == 12);
    CHECK(p.read_only);
    CHECK(p.remote_addr == "10.0.0.9");
    CHECK(p.authenticated);

    NewToken all;
    all.scopes = {"*"};
    const auto star = env.create("star", all);
    const auto star_out = env.auth->authenticate(request_with(star.token));
    REQUIRE(star_out.ok);
    CHECK(star_out.principal.all_scopes);
}

TEST_CASE("auth: posture warnings", "[auth][authn]") {
    auto joined = [](const std::vector<std::string>& w) {
        std::string s;
        for (const auto& x : w) s += x + "\n";
        return s;
    };
    {
        Env env;
        const auto text = joined(env.auth->posture_warnings());
        CHECK(text.find("no access tokens configured") != std::string::npos);
        CHECK(text.find("proxy secret not set") != std::string::npos);
    }
    {
        Env env("secret");
        NewToken star;
        star.scopes = {"*"};
        env.create("everything", star);
        NewToken expiring;
        expiring.expires = env.clock.now + std::chrono::hours(24);
        env.create("expiring", expiring);
        const auto text = joined(env.auth->posture_warnings());
        CHECK(text.find("no access tokens") == std::string::npos);
        CHECK(text.find("proxy secret") == std::string::npos);
        CHECK(text.find("without an expiry: everything") != std::string::npos);
        CHECK(text.find("expiring") == std::string::npos);
        CHECK(text.find("all scopes (*): everything") != std::string::npos);
    }
    {
        Env env("secret");
        env.create("only");
        env.store().revoke("only");
        CHECK(joined(env.auth->posture_warnings()).find("no access tokens configured") != std::string::npos);
    }
    {
        Env env;
        AuthConfig cfg = env.config;
        cfg.insecure_no_auth = true;
        TokenAuthenticator insecure(cfg);
        CHECK(joined(insecure.posture_warnings()).find("AUTHENTICATION DISABLED") != std::string::npos);
        const auto out = insecure.authenticate(request_with(""));
        CHECK(out.ok);
        CHECK_FALSE(out.principal.authenticated);
    }
}

TEST_CASE("auth: make_default_authenticator is the phase 1 seam", "[auth][authn]") {
    Env env;
    AuthConfig cfg = env.config;
    std::unique_ptr<mcp::IAuthenticator> a = make_default_authenticator(cfg);
    REQUIRE(a);
    CHECK(a->authenticate(request_with("")).error_code == "AUTH_REQUIRED");
    const auto c = env.create("ci");
    CHECK(a->authenticate(request_with(c.token)).ok);
}

// ---- backend adapter -------------------------------------------------------------------------
TEST_CASE("auth: CredentialStoreBackend works over any cred::CredentialStore", "[auth][backend]") {
    auto mem = std::make_shared<cred::InMemoryCredentialStore>();
    CredentialStoreBackend backend(mem);
    CHECK_FALSE(backend.get("a"));
    backend.put("a", "{\"x\":1}");
    backend.put("b", "two");
    CHECK(*backend.get("a") == "{\"x\":1}");
    CHECK(backend.list_names() == std::vector<std::string>{"a", "b"});
    backend.put("a", "changed");
    CHECK(*backend.get("a") == "changed");
    CHECK(backend.remove("a"));
    CHECK_FALSE(backend.remove("a"));

    // a full token store on top of it
    TokenStore store(std::make_shared<CredentialStoreBackend>(mem));
    NewToken t;
    t.name = "via-cred-store";
    t.scopes = {"screen"};
    const auto created = store.create(t);
    CHECK(store.find_by_id(created.meta.id));
}

TEST_CASE("cred: target names honour a configurable prefix", "[auth][backend]") {
    CHECK(cred::target_name("ci-bot", L"fairyfly-mcp:") == L"fairyfly-mcp:ci-bot");
    CHECK(cred::target_name("Bigfox") == L"fairyfly:Bigfox");
    CHECK(cred::connection_from_target(L"fairyfly-mcp:ci-bot", L"fairyfly-mcp:") == std::optional<std::string>("ci-bot"));
    CHECK_FALSE(cred::connection_from_target(L"fairyfly:Bigfox", L"fairyfly-mcp:"));
    CHECK_FALSE(cred::connection_from_target(L"fairyfly-mcp:x", L"fairyfly:"));  // the two namespaces never overlap
    CHECK(cred::connection_from_target(L"fairyfly:Bigfox") == std::optional<std::string>("Bigfox"));
}

// Manual only ([.]): touches the REAL Credential Manager, under a unique test prefix, and cleans up.
TEST_CASE("auth: real Credential Manager round trip (manual)", "[.][auth][realcred]") {
    const std::string prefix = "fairyfly-mcp-unittest-" + std::to_string(::GetCurrentProcessId()) + ":";
    auto backend = make_credential_manager_backend(prefix);
    struct Cleanup {
        std::shared_ptr<SecretBackend> b;
        ~Cleanup() { try { for (const auto& n : b->list_names()) b->remove(n); } catch (...) {} }
    } cleanup{backend};
    TokenStore store(backend);
    NewToken t;
    t.name = "manual-test";
    t.scopes = {"screen"};
    const auto created = store.create(t);
    CHECK(store.find_by_id(created.meta.id));
    CHECK(store.revoke("manual-test"));
    store.invalidate();
    CHECK(store.find_by_id(created.meta.id)->revoked);
}

// ---- CLI logic -------------------------------------------------------------------------------
TEST_CASE("auth: token CLI create defaults, output and confirmation", "[auth][cli]") {
    Env env;
    auto store = env.store();

    TokenCliArgs args;
    args.action = "create";
    args.name = "default-token";
    auto result = run_token_action(args, store);
    REQUIRE(result.status == Result::Status::Success);
    CHECK(result.data["scopes"] == nlohmann::json::array({"session", "connection", "screen"}));
    CHECK(result.data["read_only"] == true);
    const std::string token = result.data["token"];
    CHECK(parse_token(token));
    CHECK(result.data["warning"].get<std::string>().find("ONCE") != std::string::npos);

    // explicit scopes without --read-only grant what they name
    TokenCliArgs writer;
    writer.action = "create";
    writer.name = "writer";
    writer.scopes = {"element,screen"};
    writer.systems = {"A4H/001,A4H/002"};
    writer.tcodes = {"SE16, SM*"};
    writer.ips = {"10.0.0.0/8"};
    writer.rate = 5;
    writer.expires = "30d";
    result = run_token_action(writer, store);
    REQUIRE(result.status == Result::Status::Success);
    CHECK(result.data["read_only"] == false);
    CHECK(result.data["scopes"] == nlohmann::json::array({"element", "screen"}));
    CHECK(result.data["sap_systems"].size() == 2);
    CHECK(result.data["tcodes"].size() == 2);
    CHECK(result.data.contains("expires"));

    TokenCliArgs ro = writer;
    ro.name = "ro";
    ro.read_only_flag = true;
    CHECK(run_token_action(ro, store).data["read_only"] == true);

    // wildcard needs --yes
    TokenCliArgs star;
    star.action = "create";
    star.name = "star";
    star.scopes = {"*"};
    result = run_token_action(star, store);
    CHECK(result.status == Result::Status::Error);
    CHECK(result.error["code"] == "CONFIRMATION_REQUIRED");
    star.yes = true;
    CHECK(run_token_action(star, store).status == Result::Status::Success);

    // unknown family / bad expiry / duplicate
    TokenCliArgs bad = writer;
    bad.name = "bad";
    bad.scopes = {"nonsense"};
    CHECK(run_token_action(bad, store).error["code"] == "UNKNOWN_FAMILY");
    bad.scopes = {"screen"};
    bad.expires = "whenever";
    CHECK(run_token_action(bad, store).error["code"] == "INVALID_ARGUMENT");
    bad.expires = "2020-01-01";
    CHECK(run_token_action(bad, store).error["code"] == "INVALID_ARGUMENT");
    CHECK(run_token_action(args, store).error["code"] == "TOKEN_EXISTS");
}

TEST_CASE("auth: token CLI list/revoke/rotate never expose hashes or secrets", "[auth][cli]") {
    Env env;
    auto store = env.store();
    TokenCliArgs create;
    create.action = "create";
    create.name = "ci";
    const auto created = run_token_action(create, store);
    const std::string token = created.data["token"];
    const std::string secret = secret_of(token);

    TokenCliArgs list;
    list.action = "list";
    const auto listed = run_token_action(list, store);
    REQUIRE(listed.status == Result::Status::Success);
    const std::string dumped = listed.to_json().dump();
    CHECK(dumped.find(secret) == std::string::npos);
    CHECK(dumped.find("sha256") == std::string::npos);
    CHECK(dumped.find(sha256_hex(secret)) == std::string::npos);
    CHECK(dumped.find("\"token\"") == std::string::npos);
    CHECK(listed.data["count"] == 1);
    CHECK(listed.data["tokens"][0]["name"] == "ci");

    TokenCliArgs rotate;
    rotate.action = "rotate";
    rotate.name = "ci";
    const auto rotated = run_token_action(rotate, store);
    REQUIRE(rotated.status == Result::Status::Success);
    CHECK(rotated.data["token"] != token);
    CHECK(env.auth->authenticate(request_with(token)).error_code == "TOKEN_INVALID");
    CHECK(env.auth->authenticate(request_with(rotated.data["token"])).ok);

    TokenCliArgs revoke;
    revoke.action = "revoke";
    revoke.name = "ci";
    CHECK(run_token_action(revoke, store).status == Result::Status::Success);
    CHECK(env.auth->authenticate(request_with(rotated.data["token"])).error_code == "TOKEN_REVOKED");
    revoke.name = "ghost";
    CHECK(run_token_action(revoke, store).error["code"] == "TOKEN_NOT_FOUND");
    CHECK(run_token_action(rotate, store).error["code"] == "TOKEN_REVOKED");
    const auto after = run_token_action(list, store);
    CHECK(after.data["tokens"][0]["revoked"] == true);
    CHECK(after.to_json().dump().find(secret) == std::string::npos);
}

TEST_CASE("auth: --connections restriction is validated, stored, listed and reaches the principal", "[auth][token][connections]") {
    Env env;
    NewToken t;
    t.scopes = {"session"};
    t.connections = {"DEV*", "Bigfox"};
    const auto created = env.create("scoped", t);
    CHECK(created.meta.to_public_json()["connections"] == nlohmann::json::array({"DEV*", "Bigfox"}));
    REQUIRE(env.store().list().size() == 1);
    CHECK(env.store().list()[0].connections == t.connections);
    const auto outcome = env.auth->authenticate(request_with(created.token));
    REQUIRE(outcome.ok);
    CHECK(outcome.principal.connections == t.connections);
    // survives rotate
    auto store = env.store();
    store.rotate("scoped");
    CHECK(store.list()[0].connections == t.connections);

    NewToken bad;
    bad.name = "bad";
    bad.scopes = {"session"};
    bad.connections = {"has space"};
    CHECK_THROWS_AS(store.create(bad), AuthError);
    bad.connections = {"semi;colon"};
    CHECK_THROWS_AS(store.create(bad), AuthError);
    bad.connections = {std::string(65, 'a')};
    CHECK_THROWS_AS(store.create(bad), AuthError);

    // CLI: --connections is split on commas
    TokenCliArgs args;
    args.action = "create";
    args.name = "cli-scoped";
    args.scopes = {"session"};
    args.connections = {"DEV*,QAS"};
    const auto r = run_token_action(args, store);
    REQUIRE(r.status == Result::Status::Success);
    CHECK(r.data["connections"] == nlohmann::json::array({"DEV*", "QAS"}));
}

TEST_CASE("auth: --rate-family is validated, stored, and reaches the principal", "[auth][token][ratefamily]") {
    Env env;
    NewToken t;
    t.scopes = {"element", "key"};
    t.rate_families = {{"element", 10}, {"key", 5}};
    const auto created = env.create("limited", t);
    CHECK(created.meta.to_public_json()["rate_families"] == nlohmann::json({{"element", 10}, {"key", 5}}));
    REQUIRE(env.store().list().size() == 1);
    CHECK(env.store().list()[0].rate_families == t.rate_families);
    const auto outcome = env.auth->authenticate(request_with(created.token));
    REQUIRE(outcome.ok);
    CHECK(outcome.principal.rate_families == t.rate_families);
    CHECK(fairyfly::auth::rate_family_limit(outcome.principal, "element") == 10);
    CHECK(fairyfly::auth::rate_family_limit(outcome.principal, "screen") == 0);

    auto store = env.store();
    NewToken bad;
    bad.name = "bad";
    bad.scopes = {"element"};
    bad.rate_families = {{"bogus", 3}};
    try {
        store.create(bad);
        FAIL("expected UNKNOWN_FAMILY");
    } catch (const AuthError& e) {
        CHECK(e.code() == "UNKNOWN_FAMILY");
    }
    bad.rate_families = {{"element", 0}};
    CHECK_THROWS_AS(store.create(bad), AuthError);

    TokenCliArgs args;
    args.action = "create";
    args.name = "cli-limited";
    args.scopes = {"element"};
    args.rate_families = {"element=10,key=10"};
    auto r = run_token_action(args, store);
    REQUIRE(r.status == Result::Status::Success);
    CHECK(r.data["rate_families"] == nlohmann::json({{"element", 10}, {"key", 10}}));
    for (const char* wrong : {"element", "element=", "=5", "element=x", "element=5x", "element=0", "bogus=3"}) {
        args.name = std::string("w") + std::to_string(std::string(wrong).size()) + wrong[0];
        args.rate_families = {wrong};
        r = run_token_action(args, store);
        CHECK(r.status == Result::Status::Error);
    }
}

TEST_CASE("auth: a malformed stored rate_families makes the record unusable, not unlimited", "[auth][token][ratefamily]") {
    Env env;
    const auto created = env.create("limited");
    auto record = nlohmann::json::parse(env.tokens->get("limited").value());
    record["f"] = nlohmann::json({{"element", "ten"}});
    env.tokens->put("limited", record.dump());
    CHECK(env.store().list().empty());
    CHECK_FALSE(env.auth->authenticate(request_with(created.token)).ok);
}

TEST_CASE("auth: any malformed restriction field makes the record unusable, never unrestricted", "[auth][token][malformed]") {
    const std::vector<std::pair<const char*, const char*>> fields = {
        {"k", "connections"}, {"y", "sap_systems"}, {"t", "tcodes"}, {"s", "scopes"}, {"p", "allowed_ips"}};
    const std::vector<nlohmann::json> bad_values = {
        nlohmann::json("DEV*"),                        // wrong type: string
        nlohmann::json(5),                             // wrong type: number
        nlohmann::json(nullptr),                       // wrong type: null
        nlohmann::json::object(),                      // wrong type: object
        nlohmann::json::array({"DEV1", 7}),            // mixed-type array
        nlohmann::json::array({"DEV1", nullptr}),      // mixed-type array
        nlohmann::json::array({""}),                   // empty-string element
        nlohmann::json::array({"DEV1", ""}),           // empty-string element
    };
    for (const auto& field : fields) {
        for (const auto& bad : bad_values) {
            Env env;
            const auto created = env.create("t");
            auto record = nlohmann::json::parse(env.tokens->get("t").value());
            record[field.first] = bad;  // compact path
            env.tokens->put("t", record.dump());
            INFO("compact key " << field.first << " = " << bad.dump());
            CHECK(env.store().list().empty());
            CHECK_FALSE(env.auth->authenticate(request_with(created.token)).ok);

            Env env2;  // legacy long-key path
            const auto created2 = env2.create("t");
            nlohmann::json stored = env2.store().list()[0].to_stored_json();
            stored[field.second] = bad;
            env2.tokens->put("t", stored.dump());
            INFO("legacy key " << field.second << " = " << bad.dump());
            CHECK(env2.store().list().empty());
            CHECK_FALSE(env2.auth->authenticate(request_with(created2.token)).ok);
        }
    }
    // well-formed restrictions still load
    Env ok;
    NewToken t;
    t.scopes = {"screen"};
    t.connections = {"DEV*"};
    const auto good = ok.create("good", t);
    CHECK(ok.store().list().size() == 1);
    CHECK(ok.auth->authenticate(request_with(good.token)).ok);
}

// ---- allow_navigation (T-code allowlist hardening) ---------------------------------------------
TEST_CASE("auth: allow_navigation round-trips, defaults to false and is listed", "[auth][token][navigation]") {
    Env env;
    NewToken t;
    t.scopes = {"screen", "key"};
    t.tcodes = {"SE16"};
    t.allow_navigation = true;
    const auto nav = env.create("nav", t);
    t.allow_navigation = false;
    env.create("strict", t);
    NewToken plain_request;
    plain_request.scopes = {"screen"};
    const auto plain = env.create("plain", plain_request);

    auto store = env.store();
    const auto listed = store.list();
    REQUIRE(listed.size() == 3);
    CHECK(listed[0].name == "nav");
    CHECK(listed[0].allow_navigation);
    CHECK(listed[1].name == "plain");
    CHECK_FALSE(listed[1].allow_navigation);
    CHECK_FALSE(listed[2].allow_navigation);
    CHECK(listed[0].to_public_json()["allow_navigation"] == true);
    CHECK(listed[1].to_public_json()["allow_navigation"] == false);

    // compact record: key "a" only when true
    CHECK(listed[0].to_compact_json()["a"] == true);
    CHECK_FALSE(listed[1].to_compact_json().contains("a"));

    // the principal carries it; rotate keeps it
    const auto outcome = env.auth->authenticate(request_with(nav.token));
    REQUIRE(outcome.ok);
    CHECK(outcome.principal.allow_navigation);
    CHECK_FALSE(env.auth->authenticate(request_with(plain.token)).principal.allow_navigation);
    const auto rotated = store.rotate("nav");
    CHECK(store.list()[0].allow_navigation);
    CHECK(env.auth->authenticate(request_with(rotated.token)).principal.allow_navigation);
}

TEST_CASE("auth: allow_navigation survives chunked storage and legacy records read as false", "[auth][token][navigation][chunks]") {
    Env env;
    NewToken t = huge_token(200);
    t.tcodes = {"SE16"};
    t.allow_navigation = true;
    const auto created = env.create("bignav", t);
    CHECK(env.tokens->size() > 1);
    auto store = env.store();
    REQUIRE(store.list().size() == 1);
    CHECK(store.list()[0].allow_navigation);
    CHECK(store.list()[0].sap_systems == t.sap_systems);
    CHECK(env.auth->authenticate(request_with(created.token)).principal.allow_navigation);

    // legacy long-key and compact records without the field: false
    Env legacy;
    const auto old = legacy.create("old");
    auto meta = legacy.store().list()[0];
    meta.tcodes = {"SE16"};
    auto stored = meta.to_stored_json();
    stored.erase("allow_navigation");
    legacy.tokens->put("old", stored.dump());
    REQUIRE(legacy.store().list().size() == 1);
    CHECK_FALSE(legacy.store().list()[0].allow_navigation);
    auto compact = meta.to_compact_json();
    compact.erase("a");
    legacy.tokens->put("old", compact.dump());
    CHECK_FALSE(legacy.store().list()[0].allow_navigation);
    CHECK(legacy.auth->authenticate(request_with(old.token)).ok);
}

TEST_CASE("auth: allow_navigation needs a T-code allowlist", "[auth][token][navigation]") {
    Env env;
    auto store = env.store();
    NewToken t;
    t.name = "nav";
    t.scopes = {"screen"};
    t.allow_navigation = true;
    try {
        store.create(t);
        FAIL("expected INVALID_ARGUMENT");
    } catch (const AuthError& e) {
        CHECK(e.code() == "INVALID_ARGUMENT");
    }

    TokenCliArgs args;
    args.action = "create";
    args.name = "nav";
    args.allow_navigation = true;
    auto result = run_token_action(args, store);
    CHECK(result.status == Result::Status::Error);
    CHECK(result.error["code"] == "INVALID_ARGUMENT");
    CHECK(store.list().empty());

    args.tcodes = {"SE16,SM*"};
    result = run_token_action(args, store);
    REQUIRE(result.status == Result::Status::Success);
    CHECK(result.data["allow_navigation"] == true);
    args.action = "list";
    result = run_token_action(args, store);
    CHECK(result.data["tokens"][0]["allow_navigation"] == true);
}
