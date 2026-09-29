#include "include/menu_navigation.h"
#include "include/constants.h"
#include <algorithm>
#include <cctype>

namespace fairyfly::sap {

namespace {
std::string trim(const std::string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(),
        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(text.rbegin(), text.rend(),
        [](unsigned char c) { return std::isspace(c) != 0; }).base();
    return first < last ? std::string(first, last) : std::string();
}

std::string menu_text(const ComGuiElementPtr& item) {
    std::string text;
    try { text = item->get_property_string(L"Text"); } catch (const std::exception&) {}
    if (trim(text).empty()) {
        try { text = item->get_name(); } catch (const std::exception&) {}
    }
    return text;
}
} // namespace

std::string normalize_menu_label(const std::string& label) {
    std::string cleaned;
    for (unsigned char c : label) {
        if (c == '&') continue;
        cleaned.push_back(static_cast<char>(std::tolower(c)));
    }
    return trim(cleaned);
}

std::vector<std::string> split_menu_path(const std::string& path) {
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto piece = trim(path.substr(start, end == std::string::npos ? end : end - start));
        if (!piece.empty()) segments.push_back(piece);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return segments;
}

bool menu_label_matches(const std::string& actual, const std::string& wanted) {
    const auto want = normalize_menu_label(wanted);
    return !want.empty() && normalize_menu_label(actual) == want;
}

nlohmann::json read_menu_tree(const ComGuiElementPtr& node, int depth) {
    auto items = nlohmann::json::array();
    if (!node || depth >= constants::MAX_ELEMENT_DEPTH) return items;
    auto children = node->children();
    const int count = children.count();
    for (int index = 0; index < count; ++index) {
        auto child = children.item(index);
        if (!child) continue;
        nlohmann::json entry = nlohmann::json::object();
        try { entry["id"] = child->get_id(); } catch (const std::exception&) { entry["id"] = ""; }
        entry["text"] = menu_text(child);
        bool enabled = true;
        try { enabled = child->is_enabled(); } catch (const std::exception&) {}
        entry["enabled"] = enabled;
        entry["children"] = read_menu_tree(child, depth + 1);
        items.push_back(std::move(entry));
    }
    return items;
}

ComGuiElementPtr find_menu_by_path(const ComGuiElementPtr& root,
                                   const std::vector<std::string>& path) {
    if (!root || path.empty()) return nullptr;
    ComGuiElementPtr current = root;
    for (const auto& segment : path) {
        auto children = current->children();
        const int count = children.count();
        ComGuiElementPtr match;
        for (int index = 0; index < count && !match; ++index) {
            auto child = children.item(index);
            if (child && menu_label_matches(menu_text(child), segment)) match = child;
        }
        if (!match) return nullptr;
        current = match;
    }
    return current;
}

} // namespace fairyfly::sap
