#pragma once
// `fairyfly update check|apply`: the update channel (GitHub releases of DataZooDE/fairyfly) and the effects
// around update_model's decisions: download (redirects only to GitHub's hosts), SHA-256, Authenticode
// (WinVerifyTrust) and replacing the running fairyfly.exe (the old one stays as fairyfly.exe.old until the next update).
#include <filesystem>
#include <functional>
#include <string>

#include "include/update/update_model.h"
#include "http_client.hpp"  // libffl

namespace fairyfly::update {

struct CheckResult {
    std::string current, latest, page;
    bool available = false;
    Release release;
};
/// Asks the channel for the latest release. Throws std::exception on network or format errors.
CheckResult check(const std::string& current_version, const std::string& api_url = kLatestReleaseUrl);

/// One HTTP exchange (libffl's ffy::http_do; tests substitute it).
using HttpGet = std::function<ffy::HttpResponse(const ffy::HttpRequest&)>;

/// GET with redirects, each hop checked with download_host_allowed (the API URL itself is api.github.com).
/// When GitHub's asset host (a redirect target) fails with 400, 403, 408, 429 or 5xx, or the connection fails, the
/// whole chain starts again from `url` (a freshly signed asset URL), at most kFetchAttempts times, waiting
/// kFetchWaitsMs in between. The asset host has failed in bursts of seconds to minutes (HTTP 400, 2026-10-17/18).
std::string fetch(const std::string& url, size_t max_body, int max_redirects = 5);
std::string fetch(const std::string& url, size_t max_body, int max_redirects, const HttpGet& get, const std::function<void(int ms)>& wait);
inline constexpr int kFetchAttempts = 4;
inline constexpr int kFetchWaitsMs[kFetchAttempts - 1] = {2000, 5000, 10000};

/// Authenticode signature of a file (Windows); {false, ""} elsewhere.
Signature authenticode(const std::filesystem::path& file);
std::string sha256_file(const std::filesystem::path& file);  ///< lower-case hex
std::filesystem::path current_exe();

struct ApplyResult {
    Decision decision;
    std::string installed_path, previous_path;
};
/// Downloads, verifies and, when the decision is Install, swaps the executable. Never changes anything on Refuse.
/// In an MSIX build it refuses at once with store_managed(), before any download.
ApplyResult apply(const std::string& current_version, bool allow_unsigned, const std::string& api_url = kLatestReleaseUrl);

}  // namespace fairyfly::update
