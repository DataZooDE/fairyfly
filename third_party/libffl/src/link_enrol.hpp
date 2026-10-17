// Device enrolment: Google sign-in (OAuth code flow + PKCE on a loopback port) and registration of the device key.
#pragma once
#include <functional>
#include <string>

#include "link_state.hpp"

namespace ffy::link {

// Registers a fresh device key at <gateway_base>/enrol with the given Google ID token. Does not save.
State enrol(const std::string& gateway_base, const std::string& id_token, const std::string& host, const std::string& version);

// Opens the browser (open_url), waits for the loopback redirect, exchanges the code, returns the ID token.
std::string google_login(const std::string& client_id, const std::string& client_secret, const std::function<void(const std::string&)>& open_url,
                         const std::string& auth_endpoint = "https://accounts.google.com/o/oauth2/v2/auth",
                         const std::string& token_endpoint = "https://oauth2.googleapis.com/token");

void open_browser(const std::string& url);  // best effort; prints the URL as well

}  // namespace ffy::link
