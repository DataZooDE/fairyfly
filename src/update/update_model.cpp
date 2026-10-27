#include "include/update/update_model.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <vector>

namespace fairyfly::update {

using json = nlohmann::json;

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool is_hex64(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}
bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
}  // namespace

std::optional<CalVer> parse_version(const std::string& text) {
    std::string t = !text.empty() && (text[0] == 'v' || text[0] == 'V') ? text.substr(1) : text;
    // two or three dots: YYYY.MM.DD or YYYY.MM.DD.N
    std::vector<std::string> parts;
    size_t pos = 0;
    for (;;) {
        const size_t dot = t.find('.', pos);
        parts.push_back(t.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos));
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    if (parts.size() != 3 && parts.size() != 4) return std::nullopt;
    for (size_t i = 0; i < parts.size(); i++) {
        const auto& p = parts[i];
        if (p.empty() || p.size() > (i < 3 ? 4u : 5u)) return std::nullopt;
        for (char c : p)
            if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    CalVer v;
    v.year = std::stoi(parts[0]);
    v.month = std::stoi(parts[1]);
    v.day = std::stoi(parts[2]);
    if (v.year < 2000 || v.month < 1 || v.month > 12 || v.day < 1) return std::nullopt;
    static constexpr int kDays[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (v.year % 4 == 0 && v.year % 100 != 0) || v.year % 400 == 0;
    if (v.day > kDays[v.month - 1] || (v.month == 2 && v.day == 29 && !leap)) return std::nullopt;  // a real date
    if (parts.size() == 4) {
        if (parts[3][0] == '0') return std::nullopt;  // N >= 1, one spelling per version
        v.serial = std::stoi(parts[3]);
        if (v.serial > 65535) return std::nullopt;  // a Windows version field (VERSIONINFO, MSI) holds 16 bits
    }
    return v;
}

int compare(const CalVer& a, const CalVer& b) {
    if (a.year != b.year) return a.year < b.year ? -1 : 1;
    if (a.month != b.month) return a.month < b.month ? -1 : 1;
    if (a.day != b.day) return a.day < b.day ? -1 : 1;
    if (a.serial != b.serial) return a.serial < b.serial ? -1 : 1;
    return 0;
}

std::string format_version(const CalVer& v) {
    char buf[32];
    if (v.serial > 0)
        std::snprintf(buf, sizeof buf, "%04d.%02d.%02d.%d", v.year, v.month, v.day, v.serial);
    else
        std::snprintf(buf, sizeof buf, "%04d.%02d.%02d", v.year, v.month, v.day);
    return buf;
}

std::optional<Release> parse_release(const json& j, std::string& why) {
    if (!j.is_object()) { why = "not a release object"; return std::nullopt; }
    if (j.value("draft", false) || j.value("prerelease", false)) { why = "draft or pre-release"; return std::nullopt; }
    Release r;
    r.tag = j.value("tag_name", "");
    auto v = parse_version(r.tag);
    if (!v || r.tag[0] != 'v') { why = "tag '" + r.tag + "' is not vYYYY.MM.DD or vYYYY.MM.DD.N"; return std::nullopt; }
    r.version = format_version(*v);
    r.page_url = j.value("html_url", "");
    const std::string base = std::string("https://github.com/") + kRepository + "/releases/download/" + r.tag + "/";
    if (j.contains("assets") && j["assets"].is_array())
        for (const auto& a : j["assets"]) {
            if (!a.is_object()) continue;
            const std::string name = a.value("name", ""), url = a.value("browser_download_url", "");
            if (url != base + name) continue;  // only the project's own download path for this tag
            if (name == "fairyfly.exe") r.exe_url = url;
            if (name == "SHA256SUMS") r.sums_url = url;
        }
    if (r.exe_url.empty()) { why = "release " + r.tag + " has no fairyfly.exe from " + base; return std::nullopt; }
    if (r.sums_url.empty()) { why = "release " + r.tag + " has no SHA256SUMS from " + base; return std::nullopt; }
    return r;
}

bool download_host_allowed(const std::string& url) {
    if (!starts_with(url, "https://")) return false;
    const size_t start = 8;
    size_t end = url.find_first_of("/?#", start);
    std::string authority = lower(url.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (authority.find('@') != std::string::npos || authority.find('\\') != std::string::npos) return false;
    if (authority.size() > 4 && authority.compare(authority.size() - 4, 4, ":443") == 0) authority.resize(authority.size() - 4);
    if (authority == "github.com") return true;
    const std::string suffix = ".githubusercontent.com";
    return authority.size() > suffix.size() && authority.compare(authority.size() - suffix.size(), suffix.size(), suffix) == 0 &&
           authority.find(':') == std::string::npos;
}

std::optional<std::string> expected_sha256(const std::string& sums, const std::string& file_name) {
    std::istringstream in(sums);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 66) continue;
        std::string hash = line.substr(0, 64), rest = line.substr(64);
        size_t i = 0;
        while (i < rest.size() && rest[i] == ' ') i++;
        if (i == 0) continue;
        if (i < rest.size() && rest[i] == '*') i++;  // binary marker
        if (rest.substr(i) == file_name && is_hex64(hash)) return lower(hash);
    }
    return std::nullopt;
}

bool is_signed(const Signature& s) { return s.valid || s.present || !s.subject.empty(); }

std::vector<std::string> subject_values(const std::string& subject, const std::string& key) {
    std::vector<std::string> out;
    const std::string want = lower(key);
    auto trim = [](std::string v) {
        const auto b = v.find_first_not_of(" \t"), e = v.find_last_not_of(" \t");
        return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
    };
    size_t i = 0;
    const size_t n = subject.size();
    while (i < n) {
        // one attribute: key=value up to an unquoted separator
        const size_t eq = subject.find('=', i);
        if (eq == std::string::npos) break;
        const std::string k = lower(trim(subject.substr(i, eq - i)));
        size_t j = eq + 1;
        while (j < n && (subject[j] == ' ' || subject[j] == '\t')) j++;
        std::string value;
        if (j < n && subject[j] == '"') {
            for (j++; j < n; j++) {
                if (subject[j] == '"') {
                    if (j + 1 < n && subject[j + 1] == '"') { value += '"'; j++; continue; }
                    j++;
                    break;
                }
                value += subject[j];
            }
            while (j < n && subject[j] != ',' && subject[j] != ';' && subject[j] != '+') j++;  // junk after the quote
        } else {
            const size_t start = j;
            while (j < n && subject[j] != ',' && subject[j] != ';' && subject[j] != '+') j++;
            value = trim(subject.substr(start, j - start));
        }
        if (k == want) out.push_back(value);
        i = j < n ? j + 1 : n;
    }
    return out;
}

std::string normalize_name(const std::string& name) {
    std::string out;
    bool blank = false;
    for (unsigned char c : name) {
        if (std::isspace(c)) { blank = !out.empty(); continue; }
        if (blank) out += ' ';
        blank = false;
        out += static_cast<char>(std::tolower(c));
    }
    return out;
}

bool signer_matches(const Signature& s, const std::string& organization, const std::string& country) {
    if (!s.valid || normalize_name(organization).empty()) return false;
    const auto o = subject_values(s.subject, "O");
    if (o.size() != 1 || normalize_name(o[0]) != normalize_name(organization)) return false;
    if (normalize_name(country).empty()) return true;
    const auto c = subject_values(s.subject, "C");
    return c.size() == 1 && normalize_name(c[0]) == normalize_name(country);
}

Decision decide(const Facts& f) {
    auto refuse = [](std::string code, std::string msg) { return Decision{Verdict::Refuse, std::move(code), std::move(msg), {}}; };
    auto cur = parse_version(f.current_version), rel = parse_version(f.release_version);
    if (!cur || !rel) return refuse("UPDATE_BAD_VERSION", "cannot compare '" + f.current_version + "' with '" + f.release_version + "'");
    if (compare(*rel, *cur) <= 0) return {Verdict::UpToDate, "", "fairyfly " + f.current_version + " is up to date", {}};
    if (!is_hex64(f.expected_sha256)) return refuse("UPDATE_CHECKSUM_MISSING", "SHA256SUMS has no hash for fairyfly.exe");
    if (lower(f.actual_sha256) != lower(f.expected_sha256))
        return refuse("UPDATE_CHECKSUM_MISMATCH", "the download does not match SHA256SUMS; nothing was changed");
    std::string expected = std::string("O=") + kExpectedSignerOrganization;
    if (*kExpectedSignerCountry) expected += std::string(", C=") + kExpectedSignerCountry;
    if (is_signed(f.downloaded)) {
        // a signature that does not verify is never "unsigned": --allow-unsigned does not accept it
        if (!f.downloaded.valid)
            return refuse("UPDATE_SIGNATURE_INVALID", "the new fairyfly.exe has a signature that does not verify (" +
                                                          (f.downloaded.subject.empty() ? std::string("no signer") : f.downloaded.subject) +
                                                          "); nothing was changed");
        if (!signer_matches(f.downloaded))
            return refuse("UPDATE_SIGNER_UNEXPECTED", "the new fairyfly.exe is signed by '" + f.downloaded.subject + "', expected " + expected +
                                                          "; nothing was changed");
        return {Verdict::Install, "", "install fairyfly " + f.release_version + " (signed by " + expected + ")", {}};
    }
    // an unsigned release: never on a signed installation (no downgrade of trust), else only when asked
    if (is_signed(f.current))
        return refuse("UPDATE_SIGNATURE_INVALID", "the new fairyfly.exe is not signed and this installation is; nothing was changed");
    if (!f.allow_unsigned) return refuse("UPDATE_UNSIGNED", "the release is not signed; install it by hand or pass --allow-unsigned");
    Decision d{Verdict::Install, "", "install fairyfly " + f.release_version + " (unsigned, accepted with --allow-unsigned)", {}};
    d.warning = "the release is unsigned: only SHA256SUMS from the same GitHub release was checked, which does not prove who built it; "
                "signed releases (" + expected + ") are taken without --allow-unsigned";
    return d;
}

}  // namespace fairyfly::update
