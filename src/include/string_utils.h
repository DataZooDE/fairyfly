#pragma once

#include <cstddef>
#include <string>

namespace fairyfly {
namespace utils {

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
