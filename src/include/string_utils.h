#pragma once

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

} // namespace utils
} // namespace fairyfly
