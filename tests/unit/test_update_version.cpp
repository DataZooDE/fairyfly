// Calendar versions with an optional same-day release number: YYYY.MM.DD or YYYY.MM.DD.N (N >= 1), e.g. a second
// release on 2026-10-24 is 2026.10.24.1 (tag v2026.10.24.1). Order: 2026.10.24 < 2026.10.24.1 < 2026.10.24.2 <
// 2026.10.25. Pure functions of update_model (no network).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/update/update_model.h"

using namespace fairyfly::update;

namespace {
int cmp(const std::string& a, const std::string& b) {
    auto x = parse_version(a), y = parse_version(b);
    REQUIRE(x);
    REQUIRE(y);
    return compare(*x, *y);
}
nlohmann::json release(const std::string& tag) {
    const std::string base = "https://github.com/DataZooDE/fairyfly/releases/download/" + tag + "/";
    return {{"tag_name", tag},
            {"html_url", "https://github.com/DataZooDE/fairyfly/releases/tag/" + tag},
            {"prerelease", false},
            {"draft", false},
            {"assets", nlohmann::json::array({{{"name", "fairyfly.exe"}, {"browser_download_url", base + "fairyfly.exe"}},
                                              {{"name", "SHA256SUMS"}, {"browser_download_url", base + "SHA256SUMS"}}})}};
}
}  // namespace

TEST_CASE("update version: a same-day release number .N parses", "[update][version]") {
    REQUIRE(parse_version("2026.10.24.1"));
    REQUIRE(parse_version("v2026.10.24.1"));
    REQUIRE(parse_version("V2026.10.24.2"));
    REQUIRE(parse_version("2026.10.24.65535"));  // the largest value a Windows version field (and MSI revision) holds
    CHECK(cmp("2026.10.24.1", "v2026.10.24.1") == 0);
    // existing three-part versions keep working, zero padded or not
    REQUIRE(parse_version("2026.10.24"));
    REQUIRE(parse_version("v2026.09.30"));
    CHECK(cmp("2026.9.30", "2026.09.30") == 0);
    REQUIRE(parse_version("2028.02.29.1"));  // leap day
    REQUIRE(parse_version("2026.12.31"));
}

TEST_CASE("update version: ordering 2026.10.24 < 2026.10.24.1 < 2026.10.24.2 < 2026.10.24.10 < 2026.10.25", "[update][version]") {
    const std::vector<std::string> ordered = {"2026.09.30", "2026.10.23", "2026.10.24", "2026.10.24.1", "2026.10.24.2",
                                              "2026.10.24.10", "2026.10.25", "2026.11.01", "2027.01.01"};
    for (size_t i = 0; i < ordered.size(); i++)
        for (size_t j = 0; j < ordered.size(); j++) {
            INFO(ordered[i] << " vs " << ordered[j]);
            const int c = cmp(ordered[i], ordered[j]);
            if (i < j) CHECK(c < 0);
            if (i == j) CHECK(c == 0);
            if (i > j) CHECK(c > 0);
        }
    CHECK(cmp("2026.10.24.10", "2026.10.24.9") > 0);  // numeric, not text
}

TEST_CASE("update version: malformed versions and tags are refused", "[update][version]") {
    for (const char* bad : {"2026.10.24.0",        // N starts at 1: the plain date is the first release of the day
                            "2026.10.24.01",       // no leading zero: one spelling per version
                            "2026.10.24.",         // empty suffix
                            "2026.10.24..1",       //
                            "2026.10.24.1.1",      // at most four parts
                            "2026.10.24.x",        //
                            "2026.10.24.-1",       //
                            "2026.10.24.+1",       //
                            "2026.10.24.65536",    // does not fit a Windows version field
                            "2026.10.24.99999999999999999999",  // no overflow
                            "2026.10.24-1",        // not a pre-release or build suffix
                            "2026.10.24.1-beta",   //
                            "2026.10.24 ",         //
                            " 2026.10.24",         //
                            "2026.10.24.1 ",       //
                            "vv2026.10.24",        //
                            "v",                   //
                            ".2026.10.24",         //
                            "2026.10",             //
                            "2026.13.01.1",        // month out of range, suffix or not
                            "2026.10.32.1",        //
                            "2026.02.31",          // not a date
                            "2026.02.29.1",        // 2026 is no leap year
                            "2026.04.31",          //
                            "0.2.0"}) {
        INFO("'" << bad << "'");
        CHECK_FALSE(parse_version(bad));
    }
}

TEST_CASE("update version: a vYYYY.MM.DD.N release is accepted and its version is canonical", "[update][version]") {
    std::string why;
    auto r = parse_release(release("v2026.10.24.1"), why);
    INFO(why);
    REQUIRE(r);
    CHECK(r->tag == "v2026.10.24.1");
    CHECK(r->version == "2026.10.24.1");
    CHECK(r->exe_url == "https://github.com/DataZooDE/fairyfly/releases/download/v2026.10.24.1/fairyfly.exe");
    auto plain = parse_release(release("v2026.10.24"), why);
    REQUIRE(plain);
    CHECK(plain->version == "2026.10.24");
    CHECK_FALSE(parse_release(release("v2026.10.24.0"), why));
    CHECK(why.find("v2026.10.24.0") != std::string::npos);
    CHECK_FALSE(parse_release(release("2026.10.24.1"), why));  // tags start with v
    CHECK_FALSE(parse_release(release("v2026.10.24.1.2"), why));
}

TEST_CASE("update version: the install decision orders same-day releases", "[update][version]") {
    const std::string hash(64, 'a');
    Facts f;
    f.expected_sha256 = hash;
    f.actual_sha256 = hash;
    f.current = Signature{true, "CN=DataZoo GmbH"};
    f.downloaded = f.current;
    auto verdict = [&](const std::string& current, const std::string& release) {
        f.current_version = current;
        f.release_version = release;
        return decide(f).verdict;
    };
    CHECK(verdict("2026.10.24", "2026.10.24.1") == Verdict::Install);
    CHECK(verdict("2026.10.24.1", "2026.10.24.2") == Verdict::Install);
    CHECK(verdict("2026.10.24.1", "2026.10.25") == Verdict::Install);
    CHECK(verdict("2026.10.24.1", "2026.10.24.1") == Verdict::UpToDate);
    CHECK(verdict("2026.10.24.1", "2026.10.24") == Verdict::UpToDate);   // never back to the first release of the day
    CHECK(verdict("2026.10.25", "2026.10.24.9") == Verdict::UpToDate);
    CHECK(verdict("2026.10.24", "2026.10.24.0") == Verdict::Refuse);      // malformed release version
    CHECK(decide(f).code == "UPDATE_BAD_VERSION");
}
