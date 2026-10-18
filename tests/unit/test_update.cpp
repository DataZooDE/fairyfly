// `fairyfly update check|apply` (D10 install and update): the pure decisions. Network, Authenticode and the file
// replacement sit behind small functions in src/update/update_win.cpp; these tests use plain values only.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "include/update/update_model.h"
#include "include/update/updater.h"

using namespace fairyfly::update;
using nlohmann::json;

namespace {
json release(const std::string& tag, bool with_sums = true, const std::string& base = "https://github.com/DataZooDE/fairyfly/releases/download/") {
    json assets = json::array({{{"name", "fairyfly.exe"}, {"browser_download_url", base + tag + "/fairyfly.exe"}, {"size", 9000000}}});
    if (with_sums) assets.push_back({{"name", "SHA256SUMS"}, {"browser_download_url", base + tag + "/SHA256SUMS"}, {"size", 90}});
    return {{"tag_name", tag}, {"html_url", "https://github.com/DataZooDE/fairyfly/releases/tag/" + tag}, {"prerelease", false}, {"draft", false}, {"assets", assets}};
}
const std::string kHash = "4f0a5e2c8b1d6f7a9e3c2b1a0d9f8e7c6b5a4f3e2d1c0b9a8f7e6d5c4b3a2f1e";
}  // namespace

TEST_CASE("update: calendar versions compare by date", "[update]") {
    REQUIRE(parse_version("2026.10.16"));
    REQUIRE(parse_version("v2026.10.16"));
    CHECK(compare(*parse_version("2026.10.16"), *parse_version("v2026.10.16")) == 0);
    CHECK(compare(*parse_version("2026.10.16"), *parse_version("2026.9.30")) > 0);  // numeric, not text
    CHECK(compare(*parse_version("2026.1.2"), *parse_version("2026.01.10")) < 0);
    CHECK_FALSE(parse_version("2026.10"));
    CHECK_FALSE(parse_version("0.2.0-beta"));
    CHECK_FALSE(parse_version("v2026.13.01"));
    CHECK_FALSE(parse_version(""));
}

TEST_CASE("update: a GitHub release needs fairyfly.exe and SHA256SUMS from the project's own download path", "[update]") {
    std::string why;
    auto r = parse_release(release("v2026.11.02"), why);
    REQUIRE(r);
    CHECK(r->version == "2026.11.02");
    CHECK(r->exe_url == "https://github.com/DataZooDE/fairyfly/releases/download/v2026.11.02/fairyfly.exe");
    CHECK(r->sums_url == "https://github.com/DataZooDE/fairyfly/releases/download/v2026.11.02/SHA256SUMS");

    CHECK_FALSE(parse_release(release("v2026.11.02", false), why));
    CHECK(why.find("SHA256SUMS") != std::string::npos);
    CHECK_FALSE(parse_release(release("v2026.11.02", true, "https://evil.example/DataZooDE/fairyfly/releases/download/"), why));
    CHECK_FALSE(parse_release(release("v2026.11.02", true, "http://github.com/DataZooDE/fairyfly/releases/download/"), why));
    CHECK_FALSE(parse_release(release("v2026.11.02", true, "https://github.com/someone/fairyfly/releases/download/"), why));
    CHECK_FALSE(parse_release(release("nightly"), why));
    auto draft = release("v2026.11.02");
    draft["draft"] = true;
    CHECK_FALSE(parse_release(draft, why));
    auto pre = release("v2026.11.02");
    pre["prerelease"] = true;
    CHECK_FALSE(parse_release(pre, why));
    CHECK_FALSE(parse_release(json::array(), why));
}

TEST_CASE("update: downloads follow redirects only to GitHub's own hosts over https", "[update]") {
    CHECK(download_host_allowed("https://github.com/DataZooDE/fairyfly/releases/download/v1/fairyfly.exe"));
    CHECK(download_host_allowed("https://objects.githubusercontent.com/github-production-release-asset/x"));
    CHECK(download_host_allowed("https://release-assets.githubusercontent.com/github-production-release-asset/x?sig=1"));
    CHECK_FALSE(download_host_allowed("http://objects.githubusercontent.com/x"));
    CHECK_FALSE(download_host_allowed("https://objects.githubusercontent.com.evil.example/x"));
    CHECK_FALSE(download_host_allowed("https://evil.example/objects.githubusercontent.com"));
    CHECK_FALSE(download_host_allowed("https://github.com@evil.example/x"));
}

TEST_CASE("update: SHA256SUMS lookup", "[update]") {
    const std::string sums = kHash + "  fairyfly.exe\n" + std::string(64, 'a') + " *other.msi\r\n";
    CHECK(expected_sha256(sums, "fairyfly.exe") == kHash);
    CHECK(expected_sha256(sums, "other.msi") == std::string(64, 'a'));
    CHECK_FALSE(expected_sha256(sums, "fairyfly"));
    CHECK_FALSE(expected_sha256("zz  fairyfly.exe\n", "fairyfly.exe"));  // not a hash
    CHECK(expected_sha256(sums, "FAIRYFLY.EXE") == std::nullopt);
}

TEST_CASE("update: the install decision", "[update]") {
    Signature signed_by_us{true, "CN=DataZoo GmbH, O=DataZoo GmbH, C=DE"};
    Signature unsigned_build{false, ""};
    Facts f;
    f.current_version = "2026.10.16";
    f.release_version = "2026.11.02";
    f.expected_sha256 = kHash;
    f.actual_sha256 = kHash;
    f.current = signed_by_us;
    f.downloaded = signed_by_us;
    CHECK(decide(f).verdict == Verdict::Install);

    auto older = f;
    older.release_version = "2026.10.16";
    CHECK(decide(older).verdict == Verdict::UpToDate);
    older.release_version = "2026.09.30";
    CHECK(decide(older).verdict == Verdict::UpToDate);

    auto bad_hash = f;
    bad_hash.actual_sha256 = std::string(64, '0');
    CHECK(decide(bad_hash).code == "UPDATE_CHECKSUM_MISMATCH");
    auto no_hash = f;
    no_hash.expected_sha256.clear();
    CHECK(decide(no_hash).code == "UPDATE_CHECKSUM_MISSING");
    auto upper = f;
    upper.actual_sha256 = "4F0A5E2C8B1D6F7A9E3C2B1A0D9F8E7C6B5A4F3E2D1C0B9A8F7E6D5C4B3A2F1E";
    CHECK(decide(upper).verdict == Verdict::Install);  // hex case does not matter

    // a signed installation only takes a valid signature by the same signer
    auto bad_sig = f;
    bad_sig.downloaded = Signature{false, "CN=DataZoo GmbH, O=DataZoo GmbH, C=DE"};
    CHECK(decide(bad_sig).code == "UPDATE_SIGNATURE_INVALID");
    auto other_signer = f;
    other_signer.downloaded = Signature{true, "CN=Someone Else"};
    CHECK(decide(other_signer).code == "UPDATE_SIGNER_CHANGED");
    auto unsigned_new = f;
    unsigned_new.downloaded = unsigned_build;
    CHECK(decide(unsigned_new).code == "UPDATE_SIGNATURE_INVALID");
    unsigned_new.allow_unsigned = true;  // never weakens a signed installation
    CHECK(decide(unsigned_new).code == "UPDATE_SIGNATURE_INVALID");

    // an unsigned (self-built) installation: a signed release is fine; an unsigned one only when asked
    auto dev = f;
    dev.current = unsigned_build;
    CHECK(decide(dev).verdict == Verdict::Install);
    dev.downloaded = unsigned_build;
    CHECK(decide(dev).code == "UPDATE_UNSIGNED");
    dev.allow_unsigned = true;
    CHECK(decide(dev).verdict == Verdict::Install);

    auto garbage = f;
    garbage.release_version = "soon";
    CHECK(decide(garbage).verdict == Verdict::Refuse);
}

namespace {
const std::string kExe = "https://github.com/DataZooDE/fairyfly/releases/download/v2026.10.05/fairyfly.exe";
/// GitHub: every request to the download path redirects to a freshly signed release-assets URL.
struct FakeGithub {
    std::vector<int> asset_status;  // what the asset host answers, one entry per attempt (then 200)
    int download_calls = 0, asset_calls = 0;
    std::vector<std::string> urls;
    HttpGet get() {
        return [this](const ffy::HttpRequest& rq) {
            urls.push_back(rq.url);
            ffy::HttpResponse r;
            if (rq.url == kExe) {
                r.status = 302;
                r.headers["location"] = "https://release-assets.githubusercontent.com/github-production-release-asset/1/x?sig=" + std::to_string(++download_calls);
                return r;
            }
            const int n = asset_calls++;
            r.status = n < int(asset_status.size()) ? asset_status[size_t(n)] : 200;
            r.body = r.status == 200 ? "MZ..." : "";
            if (r.status == 400) r.headers["x-ms-error-code"] = "InvalidQueryParameterValue";
            return r;
        };
    }
};
}  // namespace

TEST_CASE("update: a failed download from GitHub's asset host starts again from the release URL", "[update]") {
    // seen on 2026-10-17/18: release-assets.githubusercontent.com answered 400 now and then for a fresh signed URL
    FakeGithub gh;
    gh.asset_status = {400, 503};
    std::vector<int> waited;
    CHECK(fetch(kExe, 1u << 20, 5, gh.get(), [&](int ms) { waited.push_back(ms); }) == "MZ...");
    CHECK(gh.download_calls == 3);  // a new signed URL each time
    CHECK(gh.asset_calls == 3);
    CHECK(waited == std::vector<int>{2000, 5000});
}

TEST_CASE("update: download retries are limited and the error names the asset host's reason", "[update]") {
    FakeGithub gh;
    gh.asset_status = {400, 400, 400, 400, 400};
    int waits = 0;
    try {
        fetch(kExe, 1u << 20, 5, gh.get(), [&](int) { waits++; });
        FAIL("no exception");
    } catch (const std::exception& e) {
        const std::string what = e.what();
        CHECK(what.find("HTTP 400") != std::string::npos);
        CHECK(what.find("x-ms-error-code: InvalidQueryParameterValue") != std::string::npos);
    }
    CHECK(gh.asset_calls == kFetchAttempts);
    CHECK(waits == kFetchAttempts - 1);
}

TEST_CASE("update: a 404 is final, and the API is not retried", "[update]") {
    FakeGithub gh;
    gh.asset_status = {404};
    CHECK_THROWS(fetch(kExe, 1u << 20, 5, gh.get(), [](int) {}));
    CHECK(gh.asset_calls == 1);

    int calls = 0;
    auto api = [&](const ffy::HttpRequest&) { calls++; ffy::HttpResponse r; r.status = 503; return r; };
    CHECK_THROWS(fetch("https://api.github.com/repos/DataZooDE/fairyfly/releases/latest", 1u << 20, 5, api, [](int) {}));
    CHECK(calls == 1);
}

// ---- the real update channel (hidden: needs internet; run with  unit_tests "[update-live]") ----

TEST_CASE("update live: the real latest GitHub release parses, its download follows GitHub's redirects and matches SHA256SUMS",
          "[.][update-live]") {
    auto c = check("2026.01.01");
    INFO("latest release " << c.latest << " " << c.page);
    CHECK(c.available);
    CHECK(c.release.exe_url.find("https://github.com/DataZooDE/fairyfly/releases/download/v") == 0);
    const std::string sums = fetch(c.release.sums_url, 64u << 10);  // github.com -> *.githubusercontent.com
    auto expected = expected_sha256(sums, "fairyfly.exe");
    REQUIRE(expected);
    const std::string bin = fetch(c.release.exe_url, 256u << 20);
    CHECK(bin.size() > 1000000);
    CHECK(bin.substr(0, 2) == "MZ");  // a Windows executable
    auto tmp = std::filesystem::temp_directory_path() / "ffy-update-live.exe";
    { std::ofstream(tmp, std::ios::binary).write(bin.data(), static_cast<std::streamsize>(bin.size())); }
    CHECK(sha256_file(tmp) == *expected);
    Facts f;
    f.current_version = "2026.01.01";
    f.release_version = c.latest;
    f.expected_sha256 = *expected;
    f.actual_sha256 = sha256_file(tmp);
    f.downloaded = authenticode(tmp);
    INFO("signature valid=" << f.downloaded.valid << " subject=" << f.downloaded.subject);
    f.allow_unsigned = true;  // the public releases are unsigned until the signing account exists (docs/SIGNING.md)
    CHECK(decide(f).verdict == Verdict::Install);
    std::filesystem::remove(tmp);
}
