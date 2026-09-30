#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <set>
#include <string>
#include <vector>

#include "include/setup/setup_validators.h"
#include "include/system/cert_scripts.h"
#include "include/system/powershell_runner.h"

using namespace fairyfly::setup;
using Catch::Matchers::ContainsSubstring;
namespace sys = fairyfly::sys;

TEST_CASE("setup validators: hostname", "[setup]") {
    for (const char* ok : {"mcp.example.com", "a", "srv-01.corp.local", "A1.b2"}) CHECK_FALSE(hostname_error(ok));
    for (const char* bad : {"", "-a.com", "a..com", "a.com.", "*.example.com", "exa mple.com", "a_b.com", "10.1.2.3",
                            "a.com\"; calc", "a$(calc).com", "a`b.com", "a/b", "a:8443", "\xC3\xBC" "ber.de"}) {
        INFO(bad);
        CHECK(hostname_error(bad));
    }
    CHECK(hostname_error(std::string(64, 'a') + ".com"));
    CHECK(hostname_error(std::string(254, 'a')));
}

TEST_CASE("setup validators: thumbprint", "[setup]") {
    CHECK_FALSE(thumbprint_error(std::string(40, 'a')));
    CHECK_FALSE(thumbprint_error("0123456789ABCDEF0123456789abcdef01234567"));
    CHECK(thumbprint_error(std::string(39, 'a')));
    CHECK(thumbprint_error(std::string(41, 'a')));
    CHECK(thumbprint_error(std::string(39, 'a') + "g"));
    CHECK(thumbprint_error(std::string(38, 'a') + "\"; calc"));
    CHECK(thumbprint_error(std::string(20, 'a') + " " + std::string(19, 'b')));
    CHECK(normalize_thumbprint("abcdef") == "ABCDEF");
}

TEST_CASE("setup validators: CIDR", "[setup]") {
    for (const char* ok : {"10.0.0.0/8", "192.168.1.5", "0.0.0.0/0", "255.255.255.255/32", "::1", "fe80::/10", "2001:db8::1/128"})
        CHECK_FALSE(cidr_error(ok));
    for (const char* bad : {"", "10.0.0.256", "10.0.0.0/33", "10.0.0/8", "10.0.0.0/", "10.0.0.0/8; calc", "a.b.c.d", "::g", ":::1",
                            "1:2:3:4:5:6:7:8:9", "10.0.0.0/-1", "10.0.0.0 /8", "$(calc)", "10.0.0.1,10.0.0.2"}) {
        INFO(bad);
        CHECK(cidr_error(bad));
    }
    const auto r = ipv4_range("10.1.2.3/8");
    REQUIRE(r);
    CHECK(r->address == "10.0.0.0");
    CHECK(r->mask == "255.0.0.0");
    CHECK(ipv4_range("192.168.1.5")->mask == "255.255.255.255");
    CHECK(ipv4_range("0.0.0.0/0")->mask == "0.0.0.0");
    CHECK_FALSE(ipv4_range("::1"));
}

TEST_CASE("setup validators: ports", "[setup]") {
    CHECK_FALSE(port_error(1));
    CHECK_FALSE(port_error(8443));
    CHECK_FALSE(port_error(65535));
    CHECK(port_error(0));
    CHECK(port_error(65536));
    CHECK(port_error(-1));
}

namespace {

class FakeRunner final : public sys::PowerShellRunner {
public:
    struct Call {
        std::string script;
        std::string params;
        int timeout_ms;
    };
    std::vector<Call> calls;
    sys::PsResult next;

    sys::PsResult run(const std::string& script, const std::string& params, int timeout_ms) override {
        calls.push_back({script, params, timeout_ms});
        return next;
    }
};

} // namespace

TEST_CASE("certificate and firewall scripts are constant and carry no interpolation", "[setup][ps]") {
    const auto& catalog = sys::script_catalog();
    REQUIRE(catalog.size() == 8);
    std::set<std::string> texts;
    for (const auto& [name, text] : catalog) {
        INFO(name);
        CHECK_THAT(text, ContainsSubstring("$env:FAIRYFLY_PS_PARAMS"));
        CHECK_THAT(text, ContainsSubstring("$ErrorActionPreference = 'Stop'"));
        CHECK_THAT(text, !ContainsSubstring("Invoke-Expression"));
        CHECK_THAT(text, !ContainsSubstring("iex "));
        CHECK_THAT(text, !ContainsSubstring("{{"));
        CHECK_THAT(text, !ContainsSubstring("Read-Host"));
        texts.insert(text);
    }
    CHECK(texts.size() == catalog.size());
}

TEST_CASE("PowerShell runner helper passes input only as JSON params and maps failures", "[setup][ps]") {
    FakeRunner runner;
    runner.next.out = R"({"found":false})";
    const std::string evil = "x\"; calc; $(calc) `whoami`";

    const auto j = sys::run_json_script(runner, sys::kFindSelfSigned, sys::find_self_signed_params(evil));
    CHECK(j.value("found", true) == false);
    REQUIRE(runner.calls.size() == 1);
    CHECK_THAT(runner.calls[0].script, !ContainsSubstring("calc"));
    CHECK(nlohmann::json::parse(runner.calls[0].params)["hostname"] == evil);
    CHECK(runner.calls[0].timeout_ms > 0);

    runner.next.exit_code = 1;
    runner.next.err = "Access denied\r\nat line 3";
    try {
        sys::run_json_script(runner, sys::kRemoveCert, sys::remove_cert_params(std::string(40, 'A')));
        FAIL("expected PowerShellError");
    } catch (const sys::PowerShellError& e) {
        CHECK(e.code == "PS_SCRIPT_FAILED");
        CHECK_THAT(e.message, ContainsSubstring("Access denied"));
        CHECK_THAT(e.message, !ContainsSubstring("line 3"));
    }
    runner.next = sys::PsResult{};
    runner.next.timed_out = true;
    try {
        sys::run_json_script(runner, sys::kFirewallExists, sys::firewall_rule_params("rule"));
        FAIL("expected PowerShellError");
    } catch (const sys::PowerShellError& e) {
        CHECK(e.code == "PS_SCRIPT_TIMEOUT");
    }
    runner.next = sys::PsResult{};
    runner.next.out = "not json";
    CHECK_THROWS_AS(sys::run_json_script(runner, sys::kFindCert, sys::find_cert_params("AB")), sys::PowerShellError);
}
