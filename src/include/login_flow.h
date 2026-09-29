#pragma once

#include <istream>
#include <stdexcept>
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

} // namespace fairyfly
