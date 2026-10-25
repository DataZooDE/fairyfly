// Values that used to be written in several places (ports, protocol versions, the bearer challenge, the token
// prefix) come from one definition each; these tests fail when a default drifts away from it.
#include <catch2/catch_test_macros.hpp>

#include "include/auth/token_store.h"
#include "include/config/mcp_config.h"
#include "include/config/mcp_doctor.h"
#include "include/mcp/constants.h"
#include "include/mcp/http_endpoint.h"
#include "include/mcp/http_server.h"
#include "include/mcp/protocol_session.h"
#include "include/mcp/types.h"
#include "include/setup/setup_hosts.h"

#include <algorithm>

using namespace fairyfly;

TEST_CASE("constants: every default port is the one constant", "[constants]") {
    CHECK(mcp::kDefaultHttpPort == 8383);
    CHECK(mcp::kDefaultTlsPort == 8443);
    const auto* port = config::find_key("server.port");
    REQUIRE(port != nullptr);
    REQUIRE(port->def.has_value());
    CHECK(std::get<long long>(*port->def) == mcp::kDefaultHttpPort);
    CHECK(mcp::HttpServerConfig{}.port == mcp::kDefaultHttpPort);
    CHECK(config::SetupQuery{}.port == mcp::kDefaultHttpPort);
    CHECK(config::DoctorInput{}.port == mcp::kDefaultHttpPort);
    CHECK(setup::kDefaultTlsPort == mcp::kDefaultTlsPort);
    CHECK(setup::kDefaultNoTlsPort == mcp::kDefaultHttpPort);
}

TEST_CASE("constants: protocol versions come from one table", "[constants][mcp]") {
    const auto stdio = mcp::stdio_protocol_versions();
    const auto http = mcp::http_protocol_versions();
    CHECK(stdio == std::vector<std::string>{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"});
    CHECK(http == std::vector<std::string>{mcp::kStatelessVersion, "2025-11-25", "2025-06-18"});
    CHECK(mcp::ServerOptions{}.protocol_versions == stdio);
    CHECK(mcp::HttpEndpoint::supported_versions() == http);
    // an unknown or missing request gets the newest stdio version
    CHECK(mcp::negotiate_version({}, "1999-01-01") == stdio.front());
}

TEST_CASE("constants: tokens start with the one prefix", "[constants][auth]") {
    CHECK(auth::kTokenPrefix == std::string_view("ffy_"));
    const std::string token = auth::format_token("0123abcd", "s3cr3t");
    CHECK(token.rfind(auth::kTokenPrefix, 0) == 0);
    CHECK_FALSE(auth::parse_token("abc_0123abcd_s3cr3t").has_value());
}

TEST_CASE("http: one builder for {error_code, message} responses, escaped and never cached", "[constants][mcp][http]") {
    // review 2026-10-25: http_endpoint and http_sys_server built this body in two ways (one by string concatenation)
    const mcp::HttpResponse r = mcp::http_json_error(413, "PAYLOAD_TOO_LARGE", "say \"no\"\n");
    CHECK(r.status == 413);
    CHECK(r.header("Content-Type") == "application/json");
    CHECK(r.header("Cache-Control") == "no-store");
    const auto body = nlohmann::json::parse(r.body);
    CHECK(body == nlohmann::json{{"error_code", "PAYLOAD_TOO_LARGE"}, {"message", "say \"no\"\n"}});
}

TEST_CASE("constants: one bearer challenge", "[constants][auth]") {
    CHECK(std::string(mcp::kBearerChallenge) == "Bearer realm=\"fairyfly\"");
    CHECK(std::string(mcp::kBearerChallengeInvalidToken).rfind(mcp::kBearerChallenge, 0) == 0);
}
