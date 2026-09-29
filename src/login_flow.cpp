#include "include/login_flow.h"

#include <algorithm>
#include <cctype>

namespace fairyfly {

namespace {
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool has_password_field(const nlohmann::json& screen) {
    if (!screen.is_object() || !screen.contains("elements") || !screen["elements"].is_array()) return false;
    for (const auto& element : screen["elements"]) {
        if (!element.is_object()) continue;
        const std::string id = element.value("id", "");
        if (id.ends_with("/usr/pwdRSYST-BCODE")) return true;
    }
    return false;
}
} // namespace

LoginCredentials parse_login_credentials(std::istream& input) {
    LoginCredentials credentials;
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));
        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower_key == "connection") credentials.connection = value;
        else if (key == "Username") credentials.username = value;
        else if (key == "Password") credentials.password = value;
        else if (key == "New Password") credentials.new_password = value;
        else if (key == "System ID") credentials.client = value;
        else if (key == "Language") credentials.language = value;
    }
    if (credentials.username.empty() || credentials.password.empty() ||
        credentials.client.size() != 3 ||
        !std::all_of(credentials.client.begin(), credentials.client.end(), [](unsigned char c) { return std::isdigit(c); }) ||
        credentials.language.empty()) {
        throw std::invalid_argument("Credential file needs Username, Password, a three-digit System ID, and optional Language");
    }
    return credentials;
}

bool is_sap_logon_screen(const nlohmann::json& screen) {
    return screen.is_object() && screen.value("transaction", "") == "S000" && has_password_field(screen);
}

bool sap_logon_completed(const nlohmann::json& screen) {
    return screen.is_object() && !screen.value("transaction", "").empty() &&
           screen.value("transaction", "") != "S000" && !has_password_field(screen);
}

bool sap_user_matches(const std::string& actual, const std::string& requested) {
    if (actual.empty() || requested.empty()) return false;
    if (actual.size() != requested.size()) return false;
    return std::equal(actual.begin(), actual.end(), requested.begin(),
                      [](unsigned char left, unsigned char right) {
                          return std::toupper(left) == std::toupper(right);
                      });
}

MultipleLogonPlan plan_multiple_logon(const std::string& choice, bool dialog_present, bool read_only) {
    MultipleLogonPlan plan;
    plan.choice = choice;
    if (choice != "fail" && choice != "keep" && choice != "end" && choice != "terminate") {
        plan.action = MultipleLogonPlan::Action::Refuse;
        plan.error_code = "INVALID_ARGUMENT";
        return plan;
    }
    if (!dialog_present) return plan;
    if (choice == "fail") {
        plan.action = MultipleLogonPlan::Action::Fail;
    } else if (choice == "end" && read_only) {
        plan.action = MultipleLogonPlan::Action::Refuse;
        plan.error_code = "READ_ONLY_REFUSED";
    } else {
        plan.action = MultipleLogonPlan::Action::Select;
        plan.radio_suffix = choice == "end" ? "usr/radMULTI_LOGON_OPT1"
                          : choice == "keep" ? "usr/radMULTI_LOGON_OPT2"
                                             : "usr/radMULTI_LOGON_OPT3";
    }
    return plan;
}

nlohmann::json make_multiple_logon_fail_error(const std::string& user_text, const std::string& terminal_text) {
    return {{"code", "LOGON_NOT_COMPLETED"},
            {"message", "SAP did not authenticate the requested user"},
            {"reason", "multiple_logon_dialog"},
            {"hint", "The user is already logged on; the multiple-logon dialog is still open. "
                     "Retry with --multiple-logon keep (continue without ending other logons), "
                     "--multiple-logon end (DESTRUCTIVE: ends the other logons, unsaved data is lost) "
                     "or --multiple-logon terminate (leave cleanly)."},
            {"dialog", {{"user", user_text}, {"terminal", terminal_text}}}};
}

nlohmann::json make_multiple_logon_annotation(const std::string& choice) {
    nlohmann::json info = {{"detected", true}, {"choice", choice}};
    if (choice == "end") info["warning"] = "other logons of this user were ended";
    return info;
}

} // namespace fairyfly
