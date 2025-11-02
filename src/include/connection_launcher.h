#pragma once

#include <string>
#include <optional>
#include <memory>
#include "com/wrapper.h"

namespace fairyfly {
namespace sap {

/// Handles SAP connection launching via sapshcut.exe
/// Extracted from ComAutomationEngine to improve separation of concerns
class ConnectionLauncher {
public:
    /// Credentials structure for SAP connection
    struct Credentials {
        std::string system_id;
        std::string client;
        std::string username;
        std::string password;
        std::string instance;
    };

    /// Constructor - takes application reference for session polling
    explicit ConnectionLauncher(ComGuiApplicationPtr app);

    /// Read credentials from trial.env file
    static std::optional<Credentials> read_credentials_from_env(const std::string& connection_name);

    /// Launch sapshcut.exe with credentials
    static bool launch_sapshcut(const std::string& connection_name, const Credentials& creds);

    /// Poll for session creation after sapshcut launch
    /// Returns the created connection and session if successful
    std::pair<ComGuiConnectionPtr, ComGuiSessionPtr> wait_for_session(
        const std::string& connection_name, 
        int timeout_seconds = 10);

private:
    ComGuiApplicationPtr app_;
};

} // namespace sap
} // namespace fairyfly

