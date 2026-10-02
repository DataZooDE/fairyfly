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

    bool exists(const std::string& name) override {
        check_name(name);
        return run(sys::kFirewallExists, sys::firewall_name_params(name)).value("exists", false);
    }
    bool display_name_exists(const std::string& display_name) override {
        check_display(display_name);
        return run(sys::kFirewallDisplayExists, sys::firewall_display_params(display_name)).value("exists", false);
    }
    std::string ensure(const std::string& name, const std::string& display_name, int port) override {
        check_name(name);
        check_display(display_name);
        if (const auto e = port_error(port)) throw HostError("INVALID_ARGUMENT", *e);
        int name_port = 0;
        if (!firewall_name_ok(name, &name_port) || name_port != port || display_name != firewall_rule_name(port))
            throw HostError("INVALID_ARGUMENT", "rule name does not match the port");
        return run(sys::kFirewallEnsure, sys::firewall_ensure_params(name, display_name, port)).value("change", "unchanged");
    }
    bool remove(const std::string& name) override {
        check_name(name);
        return run(sys::kFirewallRemove, sys::firewall_name_params(name)).value("removed", false);
    }

private:
    // Only the names fairyfly generates are ever handled ("fairyfly-mcp-https-<port>-<8 hex>", "fairyfly MCP HTTPS <port>").
    static void check_name(const std::string& name) {
        if (!firewall_name_ok(name)) throw HostError("INVALID_ARGUMENT", "unexpected firewall rule name");
    }
    static void check_display(const std::string& display_name) {
        static const std::string prefix = "fairyfly MCP HTTPS ";
        if (display_name.rfind(prefix, 0) != 0 || display_name.size() == prefix.size() || display_name.size() > prefix.size() + 5)
            throw HostError("INVALID_ARGUMENT", "unexpected firewall display name");
        for (size_t i = prefix.size(); i < display_name.size(); ++i)
            if (display_name[i] < '0' || display_name[i] > '9') throw HostError("INVALID_ARGUMENT", "unexpected firewall display name");
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
