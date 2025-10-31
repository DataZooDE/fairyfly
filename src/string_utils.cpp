#include "include/string_utils.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <sstream>
#include <iomanip>

namespace fairyfly {
namespace utils {

std::string slugify(const std::string& text) {
    std::string result;
    result.reserve(text.size());

    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == ' ' || c == '-' || c == '_') {
            result += '_';
        }
        // Skip other special characters
    }

    // Remove consecutive underscores
    auto new_end = std::unique(result.begin(), result.end(),
        [](char a, char b) { return a == '_' && b == '_'; });
    result.erase(new_end, result.end());

    // Trim leading underscores
    while (!result.empty() && result.front() == '_') {
        result.erase(result.begin());
    }

    // Trim trailing underscores
    while (!result.empty() && result.back() == '_') {
        result.pop_back();
    }

    // If empty after processing, use default
    if (result.empty()) {
        result = "screenshot";
    }

    return result;
}

std::string generate_screenshot_filename(const std::string& title) {
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;

    #ifdef _WIN32
        localtime_s(&tm, &time_t);
    #else
        localtime_r(&time_t, &tm);
    #endif

    // Format: YYYYMMDDHHMMSS
    std::ostringstream timestamp_stream;
    timestamp_stream << std::put_time(&tm, "%Y%m%d%H%M%S");
    std::string timestamp = timestamp_stream.str();

    // Generate random 4-digit number
    static bool seeded = false;
    if (!seeded) {
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        seeded = true;
    }
    int random_num = std::rand() % 10000;

    std::ostringstream random_stream;
    random_stream << std::setw(4) << std::setfill('0') << random_num;
    std::string random_str = random_stream.str();

    // Combine: slugified_title_timestamp_random.png
    std::string slug = slugify(title);
    return slug + "_" + timestamp + "_" + random_str + ".png";
}

} // namespace utils
} // namespace fairyfly
