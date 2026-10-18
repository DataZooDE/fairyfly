#pragma once
// `fairyfly update check|apply`: the update channel (GitHub releases of DataZooDE/fairyfly) and the effects
// around update_model's decisions: download (redirects only to GitHub's hosts), SHA-256, Authenticode
// (WinVerifyTrust) and replacing the running fairyfly.exe (the old one stays as fairyfly.exe.old until the next update).
#include <filesystem>
#include <string>

#include "include/update/update_model.h"

namespace fairyfly::update {

struct CheckResult {
    std::string current, latest, page;
    bool available = false;
    Release release;
};
/// Asks the channel for the latest release. Throws std::exception on network or format errors.
CheckResult check(const std::string& current_version, const std::string& api_url = kLatestReleaseUrl);

/// GET with redirects, each hop checked with download_host_allowed (the API URL itself is api.github.com).
std::string fetch(const std::string& url, size_t max_body, int max_redirects = 5);

/// Authenticode signature of a file (Windows); {false, ""} elsewhere.
Signature authenticode(const std::filesystem::path& file);
std::string sha256_file(const std::filesystem::path& file);  ///< lower-case hex
std::filesystem::path current_exe();

struct ApplyResult {
    Decision decision;
    std::string installed_path, previous_path;
};
/// Downloads, verifies and, when the decision is Install, swaps the executable. Never changes anything on Refuse.
ApplyResult apply(const std::string& current_version, bool allow_unsigned, const std::string& api_url = kLatestReleaseUrl);

}  // namespace fairyfly::update
