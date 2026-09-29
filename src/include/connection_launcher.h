#pragma once

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include "com/wrapper.h"

namespace fairyfly {
namespace sap {

/// Handles SAP connection launching via sapshcut.exe
/// Extracted from ComAutomationEngine to improve separation of concerns
class ConnectionLauncher {
public:
    enum class WaitFailure { None, Timeout, SecurityPrompt };
    struct ExistingSession {
        std::string id;
        std::string server_key;
        ComGuiSessionPtr object;
    };

    /// Constructor - takes application reference for session polling
    explicit ConnectionLauncher(ComGuiApplicationPtr app,
                                std::function<bool()> security_prompt_detector = {});
    WaitFailure wait_failure() const { return wait_failure_; }

    /// Launch the selected SAP Logon entry without putting credentials in a process command line.
    static bool launch_sapshcut(const std::string& connection_name);

    /// NOTE: credentials never go on this command line (no -pw/-user); `launch --login`
    /// logs on afterwards through the scripting API once the session is ready.
    /// Build the sapshcut command line for a logon screen, using Windows quoting rules.
    static std::wstring sapshcut_command_line(const std::wstring& executable_path,
                                              const std::string& connection_name);

    /// Quote one argument for the Windows command-line parsing rules.
    static std::wstring quote_windows_argument(const std::wstring& value);

    /// Match a SAP Logon entry to a GUI connection description.
    static bool matches_connection_name(const std::string& requested,
                                        const std::string& description);

    /// Compare backend identity before GUI collection paths, which SAP can reuse.
    static bool was_present_before_launch(const std::string& previous_id,
                                          const std::string& previous_server_key,
                                          const std::string& candidate_id,
                                          const std::string& candidate_server_key,
                                          bool same_com_object);

    /// Poll for session creation after sapshcut launch
    /// Returns a session from the requested connection if successful
    std::pair<ComGuiConnectionPtr, ComGuiSessionPtr> wait_for_session(
        const std::string& connection_name,
        int timeout_seconds,
        const std::vector<ExistingSession>& existing_sessions);

private:
    ComGuiApplicationPtr app_;
    std::function<bool()> security_prompt_detector_;
    WaitFailure wait_failure_ = WaitFailure::None;
};

} // namespace sap
} // namespace fairyfly

