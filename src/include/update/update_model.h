#pragma once
// `fairyfly update check|apply` (D10 install and update): the decisions, without network or OS calls.
// The update channel is the project's GitHub releases (the same place release.yml publishes to). A release is
// installed only when:
//   - it is newer (calendar version YYYY.MM.DD);
//   - fairyfly.exe matches its line in SHA256SUMS;
//   - its Authenticode signature is valid and, on a signed installation, made by the same signer.
// An unsigned (self-built) installation takes an unsigned release only with --allow-unsigned.
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace fairyfly::update {

struct CalVer {
    int year = 0, month = 0, day = 0;
};
/// "2026.10.16" or "v2026.10.16"; nullopt for anything else.
std::optional<CalVer> parse_version(const std::string& text);
int compare(const CalVer& a, const CalVer& b);  ///< <0, 0, >0

inline constexpr const char* kRepository = "DataZooDE/fairyfly";
inline constexpr const char* kLatestReleaseUrl = "https://api.github.com/repos/DataZooDE/fairyfly/releases/latest";

struct Release {
    std::string tag, version, exe_url, sums_url, page_url;
};
/// From GitHub's release JSON. Refuses drafts, pre-releases, other tags and assets outside
/// https://github.com/DataZooDE/fairyfly/releases/download/; `why` says what was missing.
std::optional<Release> parse_release(const nlohmann::json& github_release, std::string& why);

/// Downloads (and their redirects) only go to github.com or GitHub's asset hosts, over https.
bool download_host_allowed(const std::string& url);

/// The hash for `file_name` in a `sha256sum` style file ("<64 hex>  name" or "<64 hex> *name"), lower case.
std::optional<std::string> expected_sha256(const std::string& sums, const std::string& file_name);

struct Signature {
    bool valid = false;       ///< WinVerifyTrust accepted the Authenticode signature (chain, time stamp)
    std::string subject;      ///< signer certificate subject, empty when unsigned
};

struct Facts {
    std::string current_version, release_version;
    std::string expected_sha256, actual_sha256;
    Signature current, downloaded;
    bool allow_unsigned = false;
};

enum class Verdict { Install, UpToDate, Refuse };
struct Decision {
    Verdict verdict = Verdict::Refuse;
    std::string code;     ///< for Refuse: UPDATE_CHECKSUM_MISMATCH, UPDATE_SIGNATURE_INVALID, ...
    std::string message;
};
Decision decide(const Facts& facts);

}  // namespace fairyfly::update
