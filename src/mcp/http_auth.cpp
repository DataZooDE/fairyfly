#include "include/mcp/authenticators.h"

namespace fairyfly::mcp {

std::function<std::unique_ptr<IAuthenticator>()> g_make_authenticator;

AuthOutcome AllowAllAuthenticator::authenticate(const AuthRequest& request) {
    AuthOutcome outcome;
    outcome.ok = true;
    outcome.http_status = 200;
    outcome.principal.name = "insecure";
    outcome.principal.all_scopes = true;
    outcome.principal.authenticated = false;
    outcome.principal.remote_addr = request.peer_addr;
    return outcome;
}

std::vector<std::string> AllowAllAuthenticator::posture_warnings() const {
    return {"AUTHENTICATION DISABLED (--insecure-no-auth): anyone who can reach this port can drive the SAP GUI session"};
}

AuthOutcome DenyAllAuthenticator::authenticate(const AuthRequest&) {
    AuthOutcome outcome;
    outcome.ok = false;
    outcome.http_status = 401;
    outcome.error_code = "AUTH_REQUIRED";
    outcome.message = "authentication required; create a token: fairyfly mcp token create <name>";
    outcome.www_authenticate = "Bearer realm=\"fairyfly\"";
    return outcome;
}

std::vector<std::string> DenyAllAuthenticator::posture_warnings() const {
    return {"no authenticator configured: every request is answered 401 AUTH_REQUIRED "
            "(create a token: fairyfly mcp token create <name>, or start with --insecure-no-auth)"};
}

std::unique_ptr<IAuthenticator> make_http_authenticator(bool insecure_no_auth) {
    if (insecure_no_auth) return std::make_unique<AllowAllAuthenticator>();
    if (g_make_authenticator) {
        if (auto made = g_make_authenticator()) return made;
    }
    return std::make_unique<DenyAllAuthenticator>();
}

} // namespace fairyfly::mcp
