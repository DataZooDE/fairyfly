// Development builds are marked: a build not made from its release tag shows <base>-dev+<short commit> (docs/SIGNING.md,
// "Development builds"); a release shows the plain base version, as the release guard expects.
#include <catch2/catch_test_macros.hpp>

#include "include/build_version.h"
#include "include/doctor_checks.h"
#include "include/version.h"

#include <regex>
#include <string>

using namespace fairyfly;

TEST_CASE("build version: a release shows the plain base version", "[version]") {
    CHECK(compose_display_version("2026.11.09", true, "268d4b9a0413df0cefb1a7a816401b49c066c8fb") == "2026.11.09");
    CHECK(compose_display_version("2026.10.24.1", true, "") == "2026.10.24.1");
}

TEST_CASE("build version: any other build shows -dev+<7 hex digits of the commit>", "[version]") {
    CHECK(compose_display_version("2026.11.09", false, "268d4b9a0413df0cefb1a7a816401b49c066c8fb") == "2026.11.09-dev+268d4b9");
    CHECK(compose_display_version("2026.10.24.1", false, "ABCDEF0123") == "2026.10.24.1-dev+abcdef0");   // lower case
    CHECK(compose_display_version("2026.11.09", false, "268d4b9") == "2026.11.09-dev+268d4b9");
    // no commit (a build without git) or something that is not a commit id: still marked, without the commit
    CHECK(compose_display_version("2026.11.09", false, "") == "2026.11.09-dev");
    CHECK(compose_display_version("2026.11.09", false, "268d4b") == "2026.11.09-dev");
    CHECK(compose_display_version("2026.11.09", false, "unknown") == "2026.11.09-dev");
    CHECK(compose_display_version("2026.11.09", false, "268d4b9 ; x") == "2026.11.09-dev+268d4b9");
    CHECK(compose_display_version("2026.11.09", false, "26-8d4b9a0") == "2026.11.09-dev");
}

TEST_CASE("build version: this binary", "[version]") {
    const std::string base = FAIRYFLY_VERSION;
    const std::string shown = display_version();
    CHECK(shown == compose_display_version(base, is_release_build(), build_commit()));
    if (is_release_build()) {
        CHECK(shown == base);
    } else {
        // what build.yml checks against `git rev-parse HEAD` for the real fairyfly.exe
        CHECK(std::regex_match(shown, std::regex(std::regex_replace(base, std::regex("\\."), "\\.") + "-dev(\\+[0-9a-f]{7})?")));
    }
    // the gateway keeps the pairing version up to 32 characters, hello's agent_version up to 40: the longest possible
    // shown version (a same-day release .99 with a commit) fits
    CHECK(compose_display_version("2026.12.31.99", false, "0123456789abcdef").size() <= 32);
    CHECK(shown.size() <= 32);
}

TEST_CASE("build version: the log header names version and kind", "[version]") {
    const std::string header = build_description();
    CHECK(header.rfind("fairyfly " + display_version() + " (", 0) == 0);
    if (is_release_build()) {
        CHECK(header == "fairyfly " + display_version() + " (release)");
    } else {
        CHECK(header.find("development build") != std::string::npos);
        if (std::string(build_commit()).size() >= 7) CHECK(header.find(build_commit()) != std::string::npos);
    }
}

TEST_CASE("doctor: the version check reports the shown version and the build kind", "[doctor][version]") {
    using fairyfly::cli::doctor::judge_version;
    const auto rel = judge_version("2026.11.09", true);
    CHECK(rel.name == "version");
    CHECK(rel.status == "info");
    CHECK(rel.message == "fairyfly 2026.11.09 (release)");
    const auto dev = judge_version("2026.11.09-dev+268d4b9", false);
    CHECK(dev.status == "info");   // informational: a development build is not a problem in itself
    CHECK(dev.message == "fairyfly 2026.11.09-dev+268d4b9 (development build, not a release)");
}
