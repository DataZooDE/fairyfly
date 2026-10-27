#pragma once
// `fairyfly update check|apply` (D10 install and update): the decisions, without network or OS calls.
// The update channel is the project's GitHub releases (the same place release.yml publishes to). A release is
// installed only when:
//   - it is newer (calendar version YYYY.MM.DD, or YYYY.MM.DD.N for the N-th further release of a day);
//   - fairyfly.exe matches its line in SHA256SUMS;
//   - its Authenticode signature is valid and made by the pinned publisher (kExpectedSignerOrganization and
//     kExpectedSignerCountry: O= and C= of the leaf certificate's subject, compared normalised, never the whole subject,
//     so a re-validated certificate with another address, OU or CN still updates).
// A signed installation never takes an unsigned release. An unsigned installation (self-built, or a 2026.10.x release
// from before signing) takes a release signed by the pinned publisher, and an unsigned one only with --allow-unsigned.
// A signature that is present but does not verify (expired without a time stamp, revoked, tampered) is refused, never
// treated as "unsigned".
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fairyfly::update {

struct CalVer {
    int year = 0, month = 0, day = 0;
    int serial = 0;  ///< N of a further release on the same day (YYYY.MM.DD.N, 1..65535); 0 = the day's first release
};
/// "2026.10.16", "v2026.10.16" or, for a further release that day, "2026.10.16.1" (N 1..65535, no leading zero);
/// nullopt for anything else.
std::optional<CalVer> parse_version(const std::string& text);
/// By date, then N: 2026.10.24 < 2026.10.24.1 < 2026.10.24.2 < 2026.10.25.  <0, 0, >0
int compare(const CalVer& a, const CalVer& b);
/// Canonical text: "2026.10.05", "2026.10.24.1".
std::string format_version(const CalVer& v);

// The publisher pin. A fork that signs its own releases sets the CMake options FAIRYFLY_UPDATE_SIGNER_ORGANIZATION and
// FAIRYFLY_UPDATE_SIGNER_COUNTRY (docs/SIGNING.md, "Updates").
#ifndef FAIRYFLY_UPDATE_SIGNER_ORGANIZATION
#define FAIRYFLY_UPDATE_SIGNER_ORGANIZATION "DataZoo GmbH"
#endif
#ifndef FAIRYFLY_UPDATE_SIGNER_COUNTRY
#define FAIRYFLY_UPDATE_SIGNER_COUNTRY "DE"
#endif
inline constexpr const char* kExpectedSignerOrganization = FAIRYFLY_UPDATE_SIGNER_ORGANIZATION;
inline constexpr const char* kExpectedSignerCountry = FAIRYFLY_UPDATE_SIGNER_COUNTRY;  ///< empty: the country is not checked

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
    std::string subject;      ///< signer certificate subject (CertNameToStr, X.500 form), empty when unsigned
    bool present = false;     ///< the file carries a signature (also an invalid one); a non-empty subject implies it
};
/// The file carries an Authenticode signature, valid or not.
bool is_signed(const Signature& s);
/// All values of attribute `key` (e.g. "O", "C"; case-insensitive) in an X.500 subject as CertNameToStr writes it:
/// "CN=a, O=\"b, c\", C=DE", RDNs separated by ',' ';' or '+', quoted values unquoted ("" -> ").
std::vector<std::string> subject_values(const std::string& subject, const std::string& key);
/// For comparing names: trimmed, inner runs of white space collapsed to one blank, ASCII lower case.
std::string normalize_name(const std::string& name);
/// A valid signature whose subject has exactly one O equal to `organization` and, when `country` is not empty, exactly
/// one C equal to it (both normalised).
bool signer_matches(const Signature& s, const std::string& organization = kExpectedSignerOrganization,
                    const std::string& country = kExpectedSignerCountry);

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
    std::string warning;  ///< for Install: something the user should know (an unsigned release)
};
Decision decide(const Facts& facts);

}  // namespace fairyfly::update
