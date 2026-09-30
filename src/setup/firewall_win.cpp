// Inbound firewall rule through the constant PowerShell scripts (New-NetFirewallRule has no native equivalent
// worth reimplementing). Rule names and ports are validated here before they travel as JSON.
#include "include/setup/setup_hosts.h"
#include "include/setup/setup_model.h"
#include "include/setup/setup_validators.h"
#include "include/system/cert_scripts.h"
#include "include/system/powershell_runner.h"

namespace fairyfly::setup {

namespace {

class RealFirewall : public Firewall {
public:
    explicit RealFirewall(sys::PowerShellRunner& runner) : ps_(runner) {}

    bool exists(const std::string& rule_name) override {
        check(rule_name);
        return run(sys::kFirewallExists, sys::firewall_rule_params(rule_name)).value("exists", false);
    }
    std::string ensure(const std::string& rule_name, int port) override {
        check(rule_name);
        if (const auto e = port_error(port)) throw HostError("INVALID_ARGUMENT", *e);
        if (rule_name != firewall_rule_name(port)) throw HostError("INVALID_ARGUMENT", "rule name does not match the port");
        return run(sys::kFirewallEnsure, sys::firewall_ensure_params(rule_name, port)).value("change", "unchanged");
    }
    bool remove(const std::string& rule_name) override {
        check(rule_name);
        return run(sys::kFirewallRemove, sys::firewall_rule_params(rule_name)).value("removed", false);
    }

private:
    static void check(const std::string& rule_name) {
        // Only the fixed "fairyfly MCP HTTPS <port>" names are ever handled.
        static const std::string prefix = "fairyfly MCP HTTPS ";
        if (rule_name.rfind(prefix, 0) != 0 || rule_name.size() == prefix.size() || rule_name.size() > prefix.size() + 5)
            throw HostError("INVALID_ARGUMENT", "unexpected firewall rule name");
        for (size_t i = prefix.size(); i < rule_name.size(); ++i)
            if (rule_name[i] < '0' || rule_name[i] > '9') throw HostError("INVALID_ARGUMENT", "unexpected firewall rule name");
    }
    nlohmann::json run(const std::string& script, const std::string& params) {
        try {
            return sys::run_json_script(ps_, script, params);
        } catch (const sys::PowerShellError& e) {
            throw HostError(e.code.empty() ? "PS_SCRIPT_FAILED" : e.code, e.message);
        }
    }
    sys::PowerShellRunner& ps_;
};

} // namespace

std::unique_ptr<Firewall> make_real_firewall(sys::PowerShellRunner& runner) { return std::make_unique<RealFirewall>(runner); }

} // namespace fairyfly::setup
