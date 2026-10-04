#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <cstddef>
#include <string>
#include <exception>

namespace fairyfly::sap {

/// Named keys with their SAP GUI VKey numbers (SendVKey). The spellings are compared after lower-casing and
/// removing blanks, '_' and '-', so "Page_Down", "page-down" and "PAGEDOWN" are the same key. SAP GUI scripting
/// has VKeys for Enter, F1-F12, Shift/Ctrl combinations and the page keys; it has none for the arrow keys or Tab
/// (those are not sent through SendVKey), so they are not accepted.
/// VKey 80 = Ctrl+Page Up (first page), 81 = Page Up, 82 = Page Down, 83 = Ctrl+Page Down (last page).
struct NamedKey {
    const char* name;  ///< canonical spelling, also the first one listed in messages
    int vkey;
};
inline const NamedKey* named_key_table(std::size_t& count) {
    static const NamedKey table[] = {
        {"enter", 0},       {"return", 0},
        {"pageup", 81},     {"pgup", 81},
        {"pagedown", 82},   {"pgdn", 82},   {"pgdown", 82},
        {"pagetop", 80},    {"pgtop", 80},  {"firstpage", 80},  {"ctrl+pageup", 80},   {"ctrl+pgup", 80},
        {"pagebottom", 83}, {"pgbottom", 83}, {"lastpage", 83}, {"ctrl+pagedown", 83}, {"ctrl+pgdn", 83},
    };
    count = sizeof(table) / sizeof(table[0]);
    return table;
}

/// Human-readable list of the accepted key names for error messages and tool descriptions.
inline std::string supported_key_names_text() {
    return "enter, f1..f12, shift+f1..shift+f12, pageup (pgup, page_up), pagedown (pgdn, page_down), pagetop "
           "(ctrl+pageup), pagebottom (ctrl+pagedown), or a raw SAP VKey number 0-99; names ignore case, blanks, '_' and '-'";
}

/// Translate a key name into a SAP GUI virtual key number.
/// Accepts "enter" (0), "f1".."f12" (1..12), "shift+f1".."shift+f12" (13..24), the page keys by name
/// (see named_key_table) or a raw integer 0..99. Case-insensitive; blanks, '_' and '-' inside names are ignored.
/// Returns nullopt for anything else.
inline std::optional<int> parse_vkey(const std::string& text) {
    std::string key;     // lower-cased, blanks removed (numbers are judged on this form: "-1" stays invalid)
    for (unsigned char c : text) {
        if (!std::isspace(c)) key.push_back(static_cast<char>(std::tolower(c)));
    }
    if (key.empty()) return std::nullopt;

    if (std::all_of(key.begin(), key.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
        if (key.size() > 2) return std::nullopt;
        return std::stoi(key);
    }

    std::string name;    // additionally without '_' and '-'
    for (char c : key)
        if (c != '_' && c != '-') name.push_back(c);
    if (name.empty()) return std::nullopt;

    std::size_t count = 0;
    const NamedKey* table = named_key_table(count);
    for (std::size_t i = 0; i < count; ++i)
        if (name == table[i].name) return table[i].vkey;

    int base = 0;
    std::string function_key = name;
    if (name.rfind("shift+", 0) == 0) {
        base = 12;
        function_key = name.substr(6);
    } else if (name.rfind("shift", 0) == 0 && name.size() > 5 && name[5] == 'f') {
        base = 12;  // "shift_f4", "shift-f4"
        function_key = name.substr(5);
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

/// Which mechanism closed (or failed to close) a popup.
enum class CloseMethod { Vkey, WindowClose, Unsupported };

struct CloseAttempt {
    CloseMethod method = CloseMethod::Vkey;
    std::string original_error;  // text of the SendVKey exception, when it threw
    std::string close_error;     // text of the Close exception, when it also threw
};

/// Try to close a popup with a key press; if SendVKey throws, fall back to the
/// window's own Close method. Never falls back to Enter, and never calls
/// close for the main window (window_index <= 0).
template <typename SendFn, typename CloseFn>
CloseAttempt attempt_close(int window_index, SendFn send_key, CloseFn close_window) {
    CloseAttempt attempt;
    try {
        send_key();
        return attempt;
    } catch (const std::exception& e) {
        attempt.original_error = e.what();
    }
    attempt.method = CloseMethod::Unsupported;
    if (window_index <= 0) return attempt;
    try {
        close_window();
        attempt.method = CloseMethod::WindowClose;
    } catch (const std::exception& e) {
        attempt.close_error = e.what();
    }
    return attempt;
}

/// Some SAP dialogs accept SendVKey without throwing but remain open. Only after observing that
/// outcome, try GuiModalWindow.Close; never call Close on the main window.
template <typename CloseFn>
CloseAttempt fallback_close_when_still_open(int before_index, int after_index, CloseAttempt attempt,
                                            CloseFn close_window) {
    if (attempt.method != CloseMethod::Vkey ||
        classify_close_outcome(before_index, after_index) != CloseOutcome::StillOpen)
        return attempt;
    try {
        close_window();
        attempt.method = CloseMethod::WindowClose;
    } catch (const std::exception& e) {
        attempt.method = CloseMethod::Unsupported;
        attempt.close_error = e.what();
    }
    return attempt;
}

} // namespace fairyfly::sap
