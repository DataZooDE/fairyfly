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
        if (key == "Username") credentials.username = value;
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

} // namespace fairyfly
