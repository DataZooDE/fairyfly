#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fairyfly {
namespace utils {

/// ASCII white space: space, \t, \n, \r, \f, \v.
constexpr bool is_ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/// A-Z -> a-z for one byte; every other byte stays as it is.
constexpr char to_lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/// 0-9, A-Z, a-z (never a non-ASCII byte, whatever the C locale says).
constexpr bool is_ascii_alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

/// A-Z -> a-z; every other byte (also non-ASCII) stays as it is, whatever the C locale says.
[[nodiscard]] inline std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = to_lower_ascii(c);
    return out;
}

/// a-z -> A-Z; every other byte stays as it is.
[[nodiscard]] inline std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

/// Without ASCII white space at both ends, as a view into `s` (no allocation).
[[nodiscard]] constexpr std::string_view trim_view(std::string_view s) {
    while (!s.empty() && is_ascii_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ascii_space(s.back())) s.remove_suffix(1);
    return s;
}

/// Without ASCII white space at both ends.
[[nodiscard]] inline std::string trim(std::string_view s) { return std::string(trim_view(s)); }

/// Equal ignoring ASCII case.
[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

/// The host part of a URL or Host header names this computer: 127.0.0.1, localhost, ::1 or [::1] (any case).
[[nodiscard]] inline bool is_loopback_host(std::string_view host) {
    return host == "127.0.0.1" || host == "::1" || host == "[::1]" || iequals(host, "localhost");
}

/// Converts a string to a filesystem-safe slug
/// - Converts to lowercase
/// - Replaces spaces and hyphens with underscores
/// - Removes consecutive underscores
/// - Trims leading/trailing underscores
/// \param text Input text to slugify
/// \return Slugified string safe for filenames
std::string slugify(const std::string& text);

/// Generates a screenshot filename with timestamp and random suffix
/// Format: {slugified_title}_YYYYMMDDHHMMSS_XXXX.png
/// where XXXX is a 4-digit random number
/// \param title Screen title or transaction code
/// \return Generated filename
std::string generate_screenshot_filename(const std::string& title);

/// Decodes the HTML entities `&lt;` `&gt;` `&amp;` `&quot;` `&#39;` (also `&apos;`, `&#x27;`) in ONE pass, so
/// `&amp;lt;` becomes `&lt;` and not `<`. Tool-call transports sometimes escape `<`, `>` and `&` in arguments:
/// the tree node key `PROG<SYST>` then arrives as `PROG&lt;SYST&gt;`. Anything else, including an `&` that does
/// not start one of these entities, is kept as is.
inline std::string unescape_html_entities(const std::string& text) {
    if (text.find('&') == std::string::npos) return text;
    static const struct { const char* entity; char ch; } kEntities[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&#x27;", '\''}, {"&apos;", '\''}};
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        bool matched = false;
        if (text[i] == '&') {
            for (const auto& e : kEntities) {
                const std::string entity = e.entity;
                if (text.compare(i, entity.size(), entity) == 0) {
                    out.push_back(e.ch);
                    i += entity.size();
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) out.push_back(text[i++]);
    }
    return out;
}

} // namespace utils
} // namespace fairyfly
