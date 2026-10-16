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

private:
    Allow def_;
    std::vector<Rule> rules_;
    std::vector<std::string> admins_, publishers_, enrol_domains_, enrol_emails_;
    int max_devices_ = 5;
};

// "*", exact "gui_screen_read", family "screen", verb "screen.read".
bool tool_allowed(const std::vector<std::string>& patterns, const std::string& tool);
bool tcode_allowed(const std::vector<std::string>& allowed, const std::string& code);  // empty list = unrestricted

}  // namespace ffy::policy
