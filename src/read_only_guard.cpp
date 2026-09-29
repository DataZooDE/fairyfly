#include "include/read_only_guard.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace fairyfly::sap {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::vector<std::string> words_of(const std::string& text) {
    std::vector<std::string> words;
    std::string current;
    for (unsigned char c : lower(text)) {
        if (std::isalnum(c)) {
            current.push_back(static_cast<char>(c));
        } else if (!current.empty()) {
            words.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) words.push_back(current);
    return words;
}

bool has(const std::vector<std::string>& words, const char* w) {
    return std::find(words.begin(), words.end(), w) != words.end();
}

bool is_data_type(const std::string& type) {
    static const char* kData[] = {"GuiTextField", "GuiCTextField", "GuiPasswordField", "GuiLabel",
                                  "GuiTextedit", "GuiSimpleContainer"};
    for (const char* t : kData) if (type == t) return true;
    return false;
}

std::string rule_for_label(const std::string& label) {
    const auto words = words_of(label);
    if (words.empty()) return {};
    static const char* kDenyWords[] = {"save", "delete", "release", "stop", "post", "activate",
                                       "lock", "unlock", "create"};
    for (const char* w : kDenyWords) {
        if (has(words, w)) return std::string("word:") + w;
    }
    if (has(words, "cancel") && has(words, "job")) return "phrase:cancel job";
    if (has(words, "execute") && has(words, "background")) return "phrase:execute in background";
    if (has(words, "password") && (has(words, "change") || has(words, "reset")))
        return "phrase:change password";
    for (size_t i = 0; i < words.size(); ++i) {
        if (words[i] != "change") continue;
        const std::string next = i + 1 < words.size() ? words[i + 1] : "";
        if (next == "layout" || next == "view" || next == "variant" || next == "display") continue;
        return "word:change";
    }
    return {};
}

} // namespace

std::string matched_read_only_rule(const std::string& element_type, const std::string& text,
                                   const std::string& tooltip, const std::string& element_id) {
    const std::string id = lower(element_id);
    for (const char* frag : {"&delete", "&save", "&release"}) {
        if (id.find(frag) != std::string::npos) return std::string("id:") + frag;
    }
    static const std::string kSave = "tbar[0]/btn[11]";
    for (size_t pos = id.find(kSave); pos != std::string::npos; pos = id.find(kSave, pos + 1)) {
        const size_t end = pos + kSave.size();
        if (end >= id.size() || !std::isdigit(static_cast<unsigned char>(id[end]))) return "id:tbar[0]/btn[11]";
    }
    if (is_data_type(element_type)) return {};
    if (auto r = rule_for_label(text); !r.empty()) return r;
    return rule_for_label(tooltip);
}

bool is_state_changing_action(const std::string& element_type, const std::string& text,
                              const std::string& tooltip, const std::string& element_id) {
    return !matched_read_only_rule(element_type, text, tooltip, element_id).empty();
}

bool is_state_changing_vkey(int vkey) {
    return vkey == 11 || vkey == 14;
}

Result make_read_only_refusal(const std::string& element_id, const std::string& element_type,
                              const std::string& text, const std::string& tooltip,
                              const std::string& rule) {
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "READ_ONLY_REFUSED";
    result.error["message"] = "Refused in read-only mode: action would change SAP state (rule " + rule + ")";
    result.error["element_id"] = element_id;
    result.error["element_type"] = element_type;
    result.error["text"] = text;
    result.error["tooltip"] = tooltip;
    result.error["rule"] = rule;
    result.error["suggestions"] = nlohmann::json::array(
        {"Run without --read-only / FAIRYFLY_READ_ONLY to perform this action"});
    return result;
}

} // namespace fairyfly::sap
