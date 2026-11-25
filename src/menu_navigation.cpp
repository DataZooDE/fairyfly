#include "include/menu_navigation.h"
#include "include/string_utils.h"
#include "include/constants.h"
#include <algorithm>
#include <cctype>

namespace fairyfly::sap {

namespace {
std::string menu_text(const ComGuiElementPtr& item) {
    std::string text;
    try { text = item->get_property_string(L"Text"); } catch (const std::exception&) {}
    if (utils::trim(text).empty()) {
        try { text = item->get_name(); } catch (const std::exception&) {}
    }
    return text;
}
} // namespace

std::string normalize_menu_label(const std::string& label) {
    std::string cleaned;
    bool blank = false;
    for (unsigned char c : label) {
        if (c == '&') continue;
        if (std::isspace(c)) { blank = true; continue; }
        if (blank && !cleaned.empty()) cleaned.push_back(' ');
        blank = false;
        cleaned.push_back(static_cast<char>(std::tolower(c)));
    }
    // "Settings..." / "Settings" + U+2026: the ellipsis marks an entry that opens a dialog, it is not part of the name
    for (bool stripped = true; stripped;) {
        stripped = false;
        if (cleaned.size() >= 3 && cleaned.compare(cleaned.size() - 3, 3, "...") == 0) {
            cleaned.resize(cleaned.size() - 3);
            stripped = true;
        } else if (cleaned.size() >= 3 && cleaned.compare(cleaned.size() - 3, 3, "\xE2\x80\xA6") == 0) {
            cleaned.resize(cleaned.size() - 3);
            stripped = true;
        }
        while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
    }
    return cleaned;
}

std::string display_menu_label(const std::string& label) {
    std::string shown;
    for (char c : label)
        if (c != '&') shown.push_back(c);
    return utils::trim(shown);
}

std::vector<std::string> split_menu_path(const std::string& path) {
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto piece = utils::trim(path.substr(start, end == std::string::npos ? end : end - start));
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
    return match_menu_path(root, path).item;
}

namespace {
size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}
}  // namespace

std::vector<std::string> closest_menu_labels(const std::string& wanted, const std::vector<std::string>& labels,
                                             size_t max) {
    const std::string want = normalize_menu_label(wanted);
    if (want.empty()) return {};
    struct Candidate { size_t distance; size_t order; std::string label; };
    std::vector<Candidate> candidates;
    const size_t allowed = std::max<size_t>(1, want.size() / 3);   // a typo or two, not another word
    for (size_t i = 0; i < labels.size(); ++i) {
        const std::string have = normalize_menu_label(labels[i]);
        if (have.empty()) continue;
        const size_t distance = edit_distance(want, have);
        if (distance <= allowed) candidates.push_back({distance, i, labels[i]});
        else if (want.size() >= 4 && have.find(want) != std::string::npos) candidates.push_back({allowed + 1, i, labels[i]});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    std::vector<std::string> closest;
    for (const auto& candidate : candidates) {
        if (closest.size() >= max) break;
        closest.push_back(candidate.label);
    }
    return closest;
}

MenuPathMatch match_menu_path(const ComGuiElementPtr& root, const std::vector<std::string>& path) {
    MenuPathMatch found;
    if (!root || path.empty()) return found;
    ComGuiElementPtr current = root;
    for (size_t level = 0; level < path.size(); ++level) {
        auto children = current->children();
        const int count = children.count();
        ComGuiElementPtr match;
        std::vector<std::string> labels;
        for (int index = 0; index < count && !match; ++index) {
            auto child = children.item(index);
            if (!child) continue;
            const std::string text = menu_text(child);
            if (menu_label_matches(text, path[level])) match = child;
            else labels.push_back(display_menu_label(text));
        }
        if (!match) {
            found.failed_index = level;
            found.available = std::move(labels);
            return found;
        }
        current = match;
    }
    found.item = current;
    return found;
}

} // namespace fairyfly::sap
