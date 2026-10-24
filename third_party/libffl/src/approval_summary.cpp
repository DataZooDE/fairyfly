#include "approval_summary.hpp"

#include <regex>

#include "util.hpp"

namespace ffy::approval {

using json = nlohmann::json;

namespace {

// Printable text: it ends up in a dialog on the desktop and in the log.
std::string printable(const std::string& in) {
    std::string out;
    for (char c : in) out += static_cast<unsigned char>(c) < 0x20 || c == 0x7f ? ' ' : c;
    return out;
}

// At most `max` bytes, keeping the start and the end (where an element's own name is), cut at character boundaries.
std::string middle_cut(const std::string& s, std::size_t max) {
    if (s.size() <= max) return s;
    auto boundary = [&](std::size_t i) {  // not inside a UTF-8 sequence
        while (i > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
        return i;
    };
    std::size_t head = boundary((max - 3) / 2), tail = boundary(s.size() - (max - 3 - head));
    return s.substr(0, head) + "..." + s.substr(tail);
}

// An element id the approver can recognise: the session prefix dropped, the window kept, the middle (user area and
// subscreens) elided, the element's own name kept ("wnd[0]/usr/sub.../ctxtVBAK-AUART" -> "wnd[0]/.../ctxtVBAK-AUART").
// An index-only name ("btn[8]") keeps its parent ("tbar[1]/btn[8]").
std::string short_id(std::string id) {
    id = printable(trim(id));
    static const std::regex session(R"(^/?app/con\[\d+\]/ses\[\d+\]/)", std::regex::icase);
    id = std::regex_replace(id, session, "");
    std::vector<std::string> segs;
    for (std::size_t a = 0;;) {
        auto b = id.find('/', a);
        segs.push_back(id.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    if (segs.size() <= 3) return middle_cut(id, 72);
    static const std::regex index_only(R"(^[A-Za-z]+\[\d+\]$)");
    std::string last = segs.back();
    if (std::regex_match(last, index_only)) last = segs[segs.size() - 2] + "/" + last;
    return middle_cut(segs.front(), 16) + "/.../" + middle_cut(last, 48);
}

// The element a call acts on (`element`, or its aliases `id` / `element_id`); `name_only` keeps only its last part
// ("wnd[0]/usr/ctxtVBAK-AUART" -> "ctxtVBAK-AUART"), for lists of fields.
std::string target_of(const json& args, bool name_only = false) {
    for (const char* k : {"element", "id", "element_id"})
        if (args.is_object() && args.contains(k) && args[k].is_string()) {
            std::string id = trim(args[k].get<std::string>());
            auto slash = id.find_last_of('/');
            if (name_only && slash != std::string::npos && slash + 1 < id.size()) return middle_cut(printable(id.substr(slash + 1)), 48);
            return short_id(id);
        }
    return "";
}

// SAP's virtual keys: 0 Enter, 1-12 F1-F12, then the same with Shift, Ctrl and Ctrl+Shift. The ones that decide what a
// popup does are named.
std::string vkey_text(const json& args) {
    if (!args.is_object() || !args.contains("vkey")) return "F12 (cancel)";  // fairyfly's default
    if (!args["vkey"].is_number_integer()) return "an invalid key";
    const int64_t v = args["vkey"].get<int64_t>();
    switch (v) {
        case 0: return "Enter (confirm)";
        case 2: return "F2";
        case 3: return "F3 (back)";
        case 8: return "F8 (execute)";
        case 11: return "Ctrl+S (save)";
        case 12: return "F12 (cancel)";
        case 15: return "Shift+F3 (exit)";
    }
    static const char* mods[] = {"", "Shift+", "Ctrl+", "Ctrl+Shift+"};
    if (v >= 1 && v <= 48) return std::string(mods[(v - 1) / 12]) + "F" + std::to_string((v - 1) % 12 + 1);
    return "VKey " + std::to_string(v);
}

std::string step_summary(const std::string& tool, const json& args) {
    auto arg = [&](const char* k) { return args.is_object() && args.contains(k) && args[k].is_string() ? printable(trim(args[k].get<std::string>())) : std::string(); };
    if (tool == "gui_transaction_start") return "start transaction " + middle_cut(arg("code"), 40);
    if (tool == "gui_element_fill") {
        // field names only, every one of them: the values are not shown, they may be business data
        if (args.is_object() && args.contains("fields") && args["fields"].is_array()) {
            std::string names;
            for (auto& f : args["fields"]) names += (names.empty() ? "" : ", ") + target_of(f, true);
            std::string out = "fill fields " + names;
            if (std::string key = arg("then_key"); !key.empty()) out += ", then press " + middle_cut(key, 40);
            return out;
        }
        return "fill field " + target_of(args);
    }
    if (tool == "gui_element_click") return "click " + target_of(args);
    if (tool == "gui_element_f4") return "open the value help of " + target_of(args);
    if (tool == "gui_key_send") return "press " + middle_cut(arg("key"), 40);
    if (tool == "gui_menu_select") return "choose menu " + middle_cut(arg("path"), 120);
    if (tool == "gui_popup_close") return "close the popup with " + vkey_text(args);
    return printable(tool.substr(0, 64));
}

}  // namespace

std::vector<std::string> steps(const std::string& tool, const json& args) {
    std::vector<std::string> out;
    if (tool == "gui_batch" && args.is_object() && args.contains("items") && args["items"].is_array()) {
        for (auto& item : args["items"]) {
            if (!item.is_object() || !item.contains("tool") || !item["tool"].is_string()) out.push_back("invalid step");
            else out.push_back(step_summary(item["tool"], item.contains("arguments") ? item["arguments"] : json::object()));
        }
        return out;
    }
    out.push_back(step_summary(tool, args));
    return out;
}

std::optional<std::string> summarize(const std::string& tool, const json& args) {
    std::string out;
    const auto list = steps(tool, args);
    if (tool == "gui_batch" && args.is_object() && args.contains("items") && args["items"].is_array()) {
        // one approval covers every step: the summary names every one, numbered, or the call is not asked for at all
        out = "run " + std::to_string(list.size()) + (list.size() == 1 ? " step: " : " steps: ");
        for (std::size_t n = 0; n < list.size(); ++n) out += (n ? "; " : "") + std::to_string(n + 1) + ". " + list[n];
    } else {
        out = list.front();
    }
    if (out.size() > kMaxSummary) return std::nullopt;
    return out;
}

}  // namespace ffy::approval
