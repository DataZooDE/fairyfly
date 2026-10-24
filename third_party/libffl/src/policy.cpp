#include "policy.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <fstream>
#include <sstream>

#include "util.hpp"

namespace ffy::policy {

bool is_dangerous(const std::string& tool) {
    static const char* d[] = {"gui_batch", "gui_credentials_list", "gui_session_launch", "gui_session_login", "gui_session_disconnect", "gui_session_lease"};
    const std::string t = lower(tool);  // whatever its spelling: tool_allowed matches names case-insensitively
    for (auto x : d)
        if (t == x) return true;
    return false;
}

nlohmann::json Grants::to_json() const {
    return {{"tools", tools}, {"tcodes", tcodes}, {"systems", systems}, {"allow_write", allow_write}, {"screenshots", screenshots}, {"approval", approval}};
}

Grants Grants::from_json(const nlohmann::json& j) {
    Grants g;
    auto list = [&](const char* k) {
        std::vector<std::string> v;
        if (j.contains(k) && j[k].is_array())
            for (auto& x : j[k])
                if (x.is_string()) v.push_back(x);
        return v;
    };
    g.tools = list("tools"); g.tcodes = list("tcodes"); g.systems = list("systems");
    g.allow_write = j.value("allow_write", false);
    g.screenshots = j.value("screenshots", false);
    g.approval = j.value("approval", std::string("device"));
    return g;
}

static bool any_of_ci(const std::string& have, const std::vector<std::string>& want) { return contains_ci(want, have); }

bool Match::matches(const Identity& id) const {
    if (!emails.empty() && !any_of_ci(id.email, emails)) return false;
    if (!domains.empty() && !any_of_ci(domain_of(id.email), domains)) return false;
    if (!clients.empty() && !any_of_ci(id.client, clients)) return false;
    if (!groups.empty()) {
        bool ok = false;
        for (auto& g : id.groups)
            if (any_of_ci(g, groups)) ok = true;
        if (!ok) return false;
    }
    return true;
}

static std::vector<std::string> ylist(const YAML::Node& n) {
    std::vector<std::string> v;
    if (n && n.IsSequence())
        for (auto x : n) v.push_back(x.as<std::string>());
    return v;
}

static Allow yallow(const YAML::Node& n) {
    Allow a;
    if (!n) return a;
    a.tools = ylist(n["tools"]); a.tcodes = ylist(n["tcodes"]); a.systems = ylist(n["systems"]);
    a.allow_write = n["allow_write"] ? n["allow_write"].as<bool>() : false;
    a.screenshots = n["screenshots"] ? n["screenshots"].as<bool>() : false;
    a.approval = n["approval"] ? n["approval"].as<std::string>() : "";
    return a;
}

Engine Engine::load(const std::string& text) {
    Engine e;
    try {
        YAML::Node root = trim(text).empty() ? YAML::Node() : YAML::Load(text);
        e.def_ = yallow(root["default"]);
        if (root["rules"])
            for (auto r : root["rules"]) {
                Rule rule;
                rule.name = r["name"] ? r["name"].as<std::string>() : "";
                if (r["match"]) {
                    rule.match.emails = ylist(r["match"]["emails"]); rule.match.domains = ylist(r["match"]["domains"]);
                    rule.match.groups = ylist(r["match"]["groups"]); rule.match.clients = ylist(r["match"]["clients"]);
                }
                rule.allow = yallow(r["allow"]);
                e.rules_.push_back(std::move(rule));
            }
        e.admins_ = ylist(root["admin_emails"]);
        e.publishers_ = ylist(root["catalog_publishers"]);
        if (root["enrol"]) {
            e.enrol_domains_ = ylist(root["enrol"]["domains"]);
            e.enrol_emails_ = ylist(root["enrol"]["emails"]);
            if (root["enrol"]["max_devices"]) e.max_devices_ = root["enrol"]["max_devices"].as<int>();
        }
    } catch (const YAML::Exception& ex) {
        throw Error(std::string("policy: ") + ex.what());
    }
    // Without default tools the agent may read and navigate (attach, start a transaction, click, keys, menus, popups,
    // F4) but not write: allow_write stays false and gui_element_fill, screenshots and the dangerous tools (credentials,
    // launch, login, batch) are not granted.
    if (e.def_.tools.empty())
        e.def_.tools = {"session.list", "session.attach", "connection.list", "screen.read", "screen.find", "element.get",
                        "element.click", "element.f4", "menu.list", "menu.select", "key.send", "popup.close",
                        "transaction.start", "gui_doctor"};
    // No policy text at all (the SaaS edition): also fill fields and run batches, and writes are allowed, but only
    // where a tenant switch caps them (builtin_default(): a tenant is read-only unless it allows writes; a gateway
    // without tenants stays read-only). Every write still needs the desktop's write mode and an approval.
    if (trim(text).empty()) {
        e.builtin_ = true;
        e.def_.tools.push_back("element.fill");
        e.def_.tools.push_back("gui_batch");
        e.def_.allow_write = true;
    }
    if (e.def_.approval.empty()) e.def_.approval = "device";
    return e;
}

Engine Engine::load_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw Error("cannot read policy file " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return load(ss.str());
}

static void merge(std::vector<std::string>& dst, const std::vector<std::string>& src) {
    for (auto& s : src)
        if (std::find(dst.begin(), dst.end(), s) == dst.end()) dst.push_back(s);
}

Grants Engine::grants(const Identity& id) const {
    Grants g;
    g.tools = def_.tools; g.tcodes = def_.tcodes; g.systems = def_.systems;
    g.allow_write = def_.allow_write; g.screenshots = def_.screenshots; g.approval = def_.approval;
    for (auto& r : rules_) {
        if (!r.match.matches(id)) continue;
        merge(g.tools, r.allow.tools); merge(g.tcodes, r.allow.tcodes); merge(g.systems, r.allow.systems);
        g.allow_write = g.allow_write || r.allow.allow_write;
        g.screenshots = g.screenshots || r.allow.screenshots;
        if (r.allow.approval == "device") g.approval = "device";  // strictest wins
    }
    return g;
}

bool Engine::is_admin(const std::string& e) const { return contains_ci(admins_, e); }
bool Engine::can_publish_catalog(const std::string& e) const { return contains_ci(publishers_, e); }
bool Engine::can_enrol(const std::string& e) const { return contains_ci(enrol_emails_, e) || contains_ci(enrol_domains_, domain_of(e)); }

bool tool_allowed(const std::vector<std::string>& patterns, const std::string& tool) {
    std::string t = lower(tool);
    for (auto& praw : patterns) {
        std::string p = lower(praw);
        if (p == t) return true;
        if (is_dangerous(t)) continue;
        if (p == "*") return true;
        if (t.rfind("gui_" + p + "_", 0) == 0) return true;
        if (p.find('.') != std::string::npos) {
            std::string q = p;
            std::replace(q.begin(), q.end(), '.', '_');
            if (t == "gui_" + q) return true;
        }
    }
    return false;
}

bool tcode_allowed(const std::vector<std::string>& allowed, const std::string& code) {
    if (allowed.empty()) return true;
    std::string c = trim(code);
    if (c.size() >= 2 && c[0] == '/' && (c[1] == 'n' || c[1] == 'N')) c = c.substr(2);
    for (auto& a : allowed)
        if (iequals(a, c)) return true;
    return false;
}

namespace {

// the command field, however its id is spelled (case, surrounding whitespace, with or without the session path)
bool is_okcd(const std::string& id) {
    std::string l = lower(trim(id));
    return l == "okcd" || (l.size() >= 5 && l.compare(l.size() - 5, 5, "/okcd") == 0);
}

// A tool's name as fairyfly spells it. Anything else (another case, whitespace, punctuation) is refused outright:
// matching is case-insensitive while the step checks compare exact names, and the two must never disagree.
bool valid_tool_name(const std::string& t) {
    if (t.empty() || t.size() > 64) return false;
    for (char c : t)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

CallCheck deny(const char* code, std::string message) {
    CallCheck r;
    r.code = code;
    r.message = std::move(message);
    return r;
}

// The arguments this check (and the approval summary) reads have the types of fairyfly's schema; a wrong type is
// refused here instead of throwing further down. Returns the problem, empty if none.
std::string argument_problem(const nlohmann::json& args) {
    if (!args.is_object()) return "the arguments must be an object";
    for (const char* k : {"code", "element", "id", "element_id", "key", "then_key", "path"})
        if (args.contains(k) && !args[k].is_string()) return std::string("'") + k + "' must be a string";
    if (args.contains("vkey") && !args["vkey"].is_number_integer()) return "'vkey' must be a whole number";
    if (args.contains("fields")) {
        if (!args["fields"].is_array()) return "'fields' must be a list of fields";
        std::size_t n = 0;
        for (auto& f : args["fields"]) {
            ++n;
            if (!f.is_object()) return "field " + std::to_string(n) + " must be an object";
            for (const char* k : {"element", "id", "element_id"})
                if (f.contains(k) && !f[k].is_string()) return "field " + std::to_string(n) + ": '" + k + "' must be a string";
        }
    }
    return "";
}

// ---- fairyfly's read-only guard, as far as it can be judged from the arguments (fairyfly: src/read_only_guard.cpp,
// src/include/vkey.h, src/cli_handler_window_actions.cpp). A call under a read-only grant runs on the desktop with
// that guard on; what it would refuse is refused here already (WRITE_DENIED), instead of being sent, metered and
// refused there. Where the guard looks at the screen (the active window for Enter, a popup button's label), this
// side cannot see it and refuses: Enter only to an explicitly named main window, popups closed by gui_popup_close.

std::optional<int> parse_vkey(const std::string& text) {  // fairyfly's sap::parse_vkey
    std::string key;
    for (unsigned char c : text)
        if (!std::isspace(c)) key.push_back(static_cast<char>(std::tolower(c)));
    if (key.empty()) return std::nullopt;
    if (std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        return key.size() > 2 ? std::nullopt : std::optional<int>(std::stoi(key));
    std::string name;
    for (char c : key)
        if (c != '_' && c != '-') name.push_back(c);
    static const std::pair<const char*, int> named[] = {
        {"enter", 0}, {"return", 0}, {"pageup", 81}, {"pgup", 81}, {"pagedown", 82}, {"pgdn", 82}, {"pgdown", 82},
        {"pagetop", 80}, {"pgtop", 80}, {"firstpage", 80}, {"ctrl+pageup", 80}, {"ctrl+pgup", 80},
        {"pagebottom", 83}, {"pgbottom", 83}, {"lastpage", 83}, {"ctrl+pagedown", 83}, {"ctrl+pgdn", 83}};
    for (auto& [n, v] : named)
        if (name == n) return v;
    int base = 0;
    std::string fk = name;
    if (name.rfind("shift+", 0) == 0) { base = 12; fk = name.substr(6); }
    else if (name.rfind("shift", 0) == 0 && name.size() > 5 && name[5] == 'f') { base = 12; fk = name.substr(5); }
    if (fk.size() >= 2 && fk.size() <= 3 && fk[0] == 'f' && std::all_of(fk.begin() + 1, fk.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        int n = std::stoi(fk.substr(1));
        if (n >= 1 && n <= 12) return base + n;
    }
    return std::nullopt;
}

// read_only_vkey_rule: F1, F3, F4, F7, F8, F12, Shift+F3, the page keys; Enter only on the main window
bool vkey_read_only_ok(int vkey, int window_index) {
    switch (vkey) {
        case 1: case 3: case 4: case 7: case 8: case 12: case 15: case 80: case 81: case 82: case 83: return true;
        case 0: return window_index == 0;
        default: return false;
    }
}

// index N of the first "wnd[N]" of an element or window id; -1 if none
int window_index(const std::string& lower_id) {
    auto p = lower_id.find("wnd[");
    if (p == std::string::npos) return -1;
    std::size_t i = p + 4;
    int v = 0;
    bool any = false;
    for (; i < lower_id.size() && std::isdigit(static_cast<unsigned char>(lower_id[i])); ++i) {
        v = v * 10 + (lower_id[i] - '0');
        any = true;
        if (v > 1000) return -1;
    }
    return any && i < lower_id.size() && lower_id[i] == ']' ? v : -1;
}

std::vector<std::string> words_of(const std::string& text) {
    std::vector<std::string> w;
    std::string cur;
    for (unsigned char c : lower(text)) {
        if (std::isalnum(c)) cur.push_back(static_cast<char>(c));
        else if (!cur.empty()) { w.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

// rule_for_label: the words that name a state-changing action
bool label_changes(const std::string& label) {
    std::string norm;  // '&' accelerators dropped, as the menu matcher does
    for (char c : label)
        if (c != '&') norm.push_back(c);
    const auto w = words_of(norm);
    auto has = [&](const char* x) { return std::find(w.begin(), w.end(), x) != w.end(); };
    for (const char* x : {"save", "delete", "release", "stop", "post", "activate", "lock", "unlock", "create"})
        if (has(x)) return true;
    if ((has("cancel") && has("job")) || (has("execute") && has("background")) || (has("password") && (has("change") || has("reset")))) return true;
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (w[i] != "change") continue;
        const std::string next = i + 1 < w.size() ? w[i + 1] : "";
        if (next != "layout" && next != "view" && next != "variant" && next != "display") return true;
    }
    return false;
}

// matched_read_only_rule on the id alone (the element's type and label are on the screen)
bool element_id_changes(const std::string& raw) {
    const std::string id = lower(trim(raw));
    for (const char* frag : {"&delete", "&save", "&release"})
        if (id.find(frag) != std::string::npos) return true;
    auto at = [&](const std::string& what) {  // what, not followed by another digit
        for (auto p = id.find(what); p != std::string::npos; p = id.find(what, p + 1))
            if (p + what.size() >= id.size() || !std::isdigit(static_cast<unsigned char>(id[p + what.size()]))) return true;
        return false;
    };
    if (at("tbar[0]/btn[11]")) return true;  // the standard Save button
    // a button of a popup: Yes/OK confirm whatever the dialog asks; only the standard Cancel is known to be safe here
    const auto leaf = id.substr(id.find_last_of('/') == std::string::npos ? 0 : id.find_last_of('/') + 1);
    if (window_index(id) > 0 && leaf.rfind("btn", 0) == 0 && !at("tbar[0]/btn[12]")) return true;
    return false;
}

bool read_only_refuses(const std::string& tool, const nlohmann::json& args) {
    auto str = [&](const char* k) { return args.contains(k) && args[k].is_string() ? args[k].get<std::string>() : std::string(); };
    if (tool == "gui_key_send") {
        auto v = parse_vkey(str("key"));
        if (!v) return false;  // not a key: fairyfly refuses it as an invalid argument
        const std::string w = lower(trim(str("window")));
        return !vkey_read_only_ok(*v, w.empty() || w == "@active" ? -1 : window_index(w));
    }
    if (tool == "gui_popup_close") {  // always acts on a popup: Enter there confirms
        const int64_t v = args.contains("vkey") ? args["vkey"].get<int64_t>() : 12;
        return v < 0 || v > 99 || !vkey_read_only_ok(int(v), 1);
    }
    if (tool == "gui_menu_select") {
        const std::string path = str("path");
        for (std::size_t a = 0; a <= path.size();) {
            auto b = path.find('/', a);
            if (b == std::string::npos) b = path.size();
            if (label_changes(path.substr(a, b - a))) return true;
            a = b + 1;
        }
        return false;
    }
    if (tool == "gui_element_click")
        for (const char* k : {"element", "id", "element_id"})
            if (element_id_changes(str(k))) return true;
    return false;
}

// One step: the call itself, or one item of a batch.
CallCheck check_step(const Grants& g, const std::string& tool, const nlohmann::json& args) {
    if (auto p = argument_problem(args); !p.empty()) return deny("ARGUMENT_INVALID", tool + ": " + p);
    CallCheck r;
    r.read_only_refused = read_only_refuses(tool, args);
    if (tool == "gui_transaction_start" && !tcode_allowed(g.tcodes, args.value("code", std::string()))) {
        r.code = "TCODE_DENIED";
        r.message = "this transaction is not allowed for you";
    } else if (tool == "gui_screen_capture" && !g.screenshots) {
        r.code = "POLICY_DENIED";
        r.message = "screenshots are not enabled for this user";
    } else if (!g.tcodes.empty() && (tool == "gui_menu_select" || tool == "gui_key_send" || tool == "gui_popup_close")) {
        // fairyfly's --tcode rules for menus and keys (src/auth/authorize.cpp): they could leave the listed transactions
        std::string why;
        if (tool == "gui_menu_select") why = "menu paths can start other transactions; use gui_transaction_start";
        else if (tool == "gui_key_send") {
            const auto key = args.is_object() && args.contains("key") && args["key"].is_string() ? args["key"].get<std::string>() : std::string();
            if (!tcode_safe_key(key)) why = "key '" + key + "' can leave the transaction (allowed: enter, f4, f8 and the page keys)";
        } else if (args.is_object() && args.contains("vkey")) {
            const auto& v = args["vkey"];  // the popup's own default (F12, sent to the popup only) or a safe VKey number
            if (!v.is_number_integer() || (v.get<long long>() != 12 && !tcode_safe_key(std::to_string(v.get<long long>()))))
                why = "vkey " + v.dump() + " can leave the transaction (allowed: 12, the default, and 0, 4, 8, 80-83)";
        }
        if (!why.empty()) {
            r.code = "TCODE_DENIED";
            r.message = tool + " is not allowed while your transactions are restricted: " + why;
        }
    } else if (tool == "gui_element_fill") {
        r.writes = true;
        if (!g.tcodes.empty()) {
            // typing a transaction into the command field would get around the transaction list: the call's own
            // element and every field of a multi-field fill
            std::vector<const nlohmann::json*> targets{&args};
            if (args.contains("fields"))
                for (auto& f : args["fields"]) targets.push_back(&f);
            for (auto* t : targets)
                for (const char* k : {"element", "id", "element_id"})
                    if (t->contains(k) && is_okcd((*t)[k].get<std::string>())) {
                        r.code = "TCODE_DENIED";
                        r.message = "typing into the command field is not allowed while your transactions are restricted; "
                                    "use gui_transaction_start";
                    }
        }
    }
    return r;
}

}  // namespace

CallCheck check_call(const Grants& g, const std::string& tool, const nlohmann::json& args) {
    if (!valid_tool_name(tool)) return deny("TOOL_NAME_INVALID", "'" + tool.substr(0, 64) + "' is not a tool name (tool names are lower case)");
    if (tool != "gui_batch") return check_step(g, tool, args);
    CallCheck r;
    auto bad = [&](const std::string& msg) { return deny("BATCH_INVALID", msg); };
    if (!args.is_object() || !args.contains("items") || !args["items"].is_array() || args["items"].empty() ||
        args["items"].size() > kMaxBatchItems)
        return bad("gui_batch needs 'items': 1 to " + std::to_string(kMaxBatchItems) + " tool calls");
    std::size_t n = 0;
    for (auto& item : args["items"]) {
        ++n;
        const std::string at = "batch item " + std::to_string(n) + ": ";
        if (!item.is_object() || !item.contains("tool") || !item["tool"].is_string() || item["tool"].get<std::string>().empty())
            return bad(at + "needs a 'tool' name");
        const std::string t = item["tool"];
        if (!valid_tool_name(t)) return deny("TOOL_NAME_INVALID", at + "'" + t.substr(0, 64) + "' is not a tool name (tool names are lower case)");
        const nlohmann::json a = item.contains("arguments") ? item["arguments"] : nlohmann::json::object();
        if (!a.is_object()) return bad(at + "'arguments' must be an object");
        if (t == "gui_batch") return bad(at + "gui_batch cannot be nested");
        if (is_dangerous(t) || !tool_allowed(g.tools, t)) return deny("POLICY_DENIED", at + t + " is not permitted for this user");
        auto step = check_step(g, t, a);
        if (!step.ok()) return deny(step.code.c_str(), at + step.message);
        r.writes = r.writes || step.writes;
        r.read_only_refused = r.read_only_refused || step.read_only_refused;
        r.steps.push_back(t);
        if (std::find(r.tools.begin(), r.tools.end(), t) == r.tools.end()) r.tools.push_back(t);
    }
    return r;
}

bool tcode_safe_key(const std::string& key) {
    // fairyfly's tcode_safe_key (src/auth/authorize.cpp) on the one key parser both rule sets use (parse_vkey, fairyfly's
    // src/include/vkey.h): Enter, F4, F8 and the page keys stay in the open transaction
    const auto v = parse_vkey(key);
    return v && (*v == 0 || *v == 4 || *v == 8 || (*v >= 80 && *v <= 83));
}

bool needs_desktop_tcodes(const Grants& g, const std::string& tool, const CallCheck& checked) {
    if (g.tcodes.empty()) return false;
    auto on_screen = [](const std::string& t) {  // fairyfly's acts_on_screen: the families screen, element, key, popup, menu
        for (const char* f : {"gui_screen_", "gui_element_", "gui_key_", "gui_popup_", "gui_menu_"})
            if (t.rfind(f, 0) == 0) return true;
        return false;
    };
    if (on_screen(tool)) return true;
    return std::any_of(checked.tools.begin(), checked.tools.end(), on_screen);
}

}  // namespace ffy::policy
