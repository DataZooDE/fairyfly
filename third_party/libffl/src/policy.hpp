// Cloud-side authorization rules and the grants carried in a call assertion. The same tool-matching function is
// used by the desktop agent so both sides agree.
#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ffy::policy {

// Never matched by "*" or by a family; a policy has to name them exactly.
bool is_dangerous(const std::string& tool);

struct Grants {
    std::vector<std::string> tools, tcodes, systems;
    bool allow_write = false, screenshots = false;
    std::string approval = "device";  // "device": confirm on the desktop; "none"
    nlohmann::json to_json() const;
    static Grants from_json(const nlohmann::json& j);
};

struct Identity {
    std::string email, client;
    std::vector<std::string> groups;
};

struct Allow {
    std::vector<std::string> tools, tcodes, systems;
    bool allow_write = false, screenshots = false;
    std::string approval;
};
struct Match {
    std::vector<std::string> emails, domains, groups, clients;
    bool matches(const Identity& id) const;
};
struct Rule {
    std::string name;
    Match match;
    Allow allow;
};

class Engine {
public:
    static Engine load(const std::string& yaml);  // empty text -> conservative built-in default; throws ffy::Error
    static Engine load_file(const std::string& path);
    Grants grants(const Identity& id) const;
    bool is_admin(const std::string& email) const;
    bool can_publish_catalog(const std::string& email) const;
    bool can_enrol(const std::string& email) const;
    int max_devices() const { return max_devices_; }
    // No policy text at all: the built-in default. It allows writes only where a tenant switch caps them (SaaS tenants,
    // read-only unless the tenant allows writes); a gateway without tenants keeps it read-only.
    bool builtin_default() const { return builtin_; }

private:
    Allow def_;
    bool builtin_ = false;
    std::vector<Rule> rules_;
    std::vector<std::string> admins_, publishers_, enrol_domains_, enrol_emails_;
    int max_devices_ = 5;
};

// "*", exact "gui_screen_read", family "screen", verb "screen.read".
bool tool_allowed(const std::vector<std::string>& patterns, const std::string& tool);
bool tcode_allowed(const std::vector<std::string>& allowed, const std::string& code);  // empty list = unrestricted

// What one tools/call does beyond its name, checked the same way by the gateway and the desktop: the steps of a
// gui_batch (each granted, none dangerous or nested, transactions allowed), typing into the command field while
// transactions are restricted (also in a multi-field fill), screenshots. `tools` lists the step tools of a batch.
struct CallCheck {
    std::string code, message;
    std::vector<std::string> tools;
    std::vector<std::string> steps;  // every step tool of a batch, in order (for the audit record)
    bool writes = false;  // a gui_element_fill, on its own or as a step
    bool read_only_refused = false;  // a step fairyfly's read-only guard refuses (keys, popups, menus, buttons): needs a write grant
    bool ok() const { return code.empty(); }
};
constexpr std::size_t kMaxBatchItems = 20;
CallCheck check_call(const Grants& g, const std::string& tool, const nlohmann::json& args);

// fairyfly's --tcode key rule: keys that stay in the open transaction (Enter, F4, F8, the page keys, as names or VKeys)
bool tcode_safe_key(const std::string& key);
// The call (or one of its batch steps, checked.tools) acts on the SAP screen (screen, element, key, popup, menu) while
// a transaction list applies: only a desktop that enforces the list itself (proto::kCapTcodes) may run it.
bool needs_desktop_tcodes(const Grants& g, const std::string& tool, const CallCheck& checked);

}  // namespace ffy::policy
