#pragma once

#include <istream>
#include <stdexcept>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace fairyfly {

struct LoginCredentials {
    std::string connection;  ///< optional "Connection:" key (name for the credential store)
    std::string username;
    std::string password;
    std::string new_password;
    std::string client;
    std::string language = "EN";
};

LoginCredentials parse_login_credentials(std::istream& input);
bool is_sap_logon_screen(const nlohmann::json& screen);
bool sap_logon_completed(const nlohmann::json& screen);
bool sap_user_matches(const std::string& actual, const std::string& requested);

/// Multiple-logon dialog ("License Information for Multiple Logons", wnd[1]).
/// Radio ids under wnd[1]/usr: radMULTI_LOGON_OPT1 (end other logons, destructive),
/// OPT2 (continue without ending others), OPT3 (terminate this logon, the SAP default).
struct MultipleLogonPlan {
    enum class Action { None, Fail, Select, Refuse };
    Action action = Action::None;
    std::string radio_suffix;                ///< e.g. "usr/radMULTI_LOGON_OPT2" (Select only)
    std::string choice;                      ///< the requested choice
    std::optional<std::string> error_code;   ///< set for Refuse: INVALID_ARGUMENT or READ_ONLY_REFUSED
};

/// Pure decision: choice is fail|keep|end|terminate (case-sensitive). `end` is destructive and is
/// refused under --read-only. Without the dialog the plan is None (unknown choices still Refuse).
MultipleLogonPlan plan_multiple_logon(const std::string& choice, bool dialog_present, bool read_only);

/// Error JSON for choice `fail`: LOGON_NOT_COMPLETED with reason, hint and the dialog info texts.
nlohmann::json make_multiple_logon_fail_error(const std::string& user_text, const std::string& terminal_text);

/// Success annotation `multiple_logon` for keep/end (end carries a warning).
nlohmann::json make_multiple_logon_annotation(const std::string& choice);

} // namespace fairyfly
