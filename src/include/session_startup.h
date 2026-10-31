#pragma once
#include "core.h"
#include <optional>
#include <string>

namespace fairyfly::startup {

// Startup is independent of business-write approval. A gateway request must
// carry an explicit grant; older gateways therefore cannot enable startup.
struct Policy {
    bool local_enabled = false;
    bool link_request = false;
    bool gateway_allowed = false;
    std::string default_connection;
};

// Called only when a suitable session could not be reused, before any process
// launch or login. Existing sessions never require startup permission.
inline std::optional<Result> check(const Policy& policy) {
    if (policy.link_request && !policy.gateway_allowed)
        return Result::fail("SAP_AUTOSTART_ADMIN_DISABLED",
            "Automatic SAP startup is not permitted by the gateway.",
            json::array({"Ask your organisation administrator to allow automatic SAP startup, then retry. "
                         "If the gateway is older, update it to a version supporting automatic SAP startup.",
                         "Alternatively, start SAP and log in manually, then retry the original task."}));
    if (!policy.local_enabled)
        return Result::fail("SAP_AUTOSTART_DISABLED",
            "Automatic SAP startup is disabled on this computer.",
            json::array({"Enable 'Allow automatic SAP startup' in the Fairyfly tray. "
                         "Without tray support, start Fairyfly with --allow-sap-startup.",
                         "Alternatively, start SAP and log in manually, then retry the original task."}));
    if (policy.default_connection.find_first_not_of(" \t\r\n") == std::string::npos)
        return Result::fail("SAP_AUTOSTART_DEFAULT_MISSING",
            "No local default SAP connection is configured.",
            json::array({"Run 'fairyfly session default NAME' locally with your SAP Logon connection name, then retry."}));
    return std::nullopt;
}

} // namespace fairyfly::startup
