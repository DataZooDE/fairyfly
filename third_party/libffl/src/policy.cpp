#include "policy.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "util.hpp"

namespace ffy::policy {

bool is_dangerous(const std::string& t) {
    static const char* d[] = {"gui_batch", "gui_credentials_list", "gui_session_launch", "gui_session_login", "gui_session_disconnect", "gui_session_lease"};
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
    if (e.def_.tools.empty()) e.def_.tools = {"session.list", "connection.list", "screen.read", "screen.find", "element.get", "menu.list", "gui_doctor"};
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
        if (is_dangerous(tool)) continue;
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

}  // namespace ffy::policy
