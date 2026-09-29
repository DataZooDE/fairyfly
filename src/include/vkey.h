#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace fairyfly::sap {

/// Translate a key name into a SAP GUI virtual key number.
/// Accepts "enter" (0), "f1".."f12" (1..12), "shift+f1".."shift+f12" (13..24)
/// or a raw integer 0..99. Case-insensitive; surrounding blanks are ignored.
/// Returns nullopt for anything else.
inline std::optional<int> parse_vkey(const std::string& text) {
    std::string key;
    for (unsigned char c : text) {
        if (!std::isspace(c)) key.push_back(static_cast<char>(std::tolower(c)));
    }
    if (key.empty()) return std::nullopt;
    if (key == "enter") return 0;

    if (std::all_of(key.begin(), key.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
        if (key.size() > 2) return std::nullopt;
        return std::stoi(key);
    }

    int base = 0;
    std::string function_key = key;
    if (key.rfind("shift+", 0) == 0) {
        base = 12;
        function_key = key.substr(6);
    }
    if (function_key.size() >= 2 && function_key.size() <= 3 && function_key[0] == 'f' &&
        std::all_of(function_key.begin() + 1, function_key.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
        const int number = std::stoi(function_key.substr(1));
        if (number >= 1 && number <= 12) return base + number;
    }
    return std::nullopt;
}

/// Outcome of a close-popup attempt, judged from the active window index
/// before and after the key press (0 = main window, N > 0 = popup).
enum class CloseOutcome { Closed, NoPopup, StillOpen };

inline CloseOutcome classify_close_outcome(int before_index, int after_index) {
    if (before_index <= 0) return CloseOutcome::NoPopup;
    return after_index < before_index ? CloseOutcome::Closed : CloseOutcome::StillOpen;
}

} // namespace fairyfly::sap
