#pragma once
// Default authenticators of the HTTP transport until the token store (phase 2) provides the real one.

#include <functional>
#include <memory>

#include "include/mcp/principal.h"

namespace fairyfly::mcp {

/// --insecure-no-auth only: every request is accepted as principal "insecure" (authenticated=false).
class AllowAllAuthenticator : public IAuthenticator {
public:
    AuthOutcome authenticate(const AuthRequest& request) override;
    std::vector<std::string> posture_warnings() const override;
};

/// Default: every request is answered 401 AUTH_REQUIRED with a hint how to create a token.
class DenyAllAuthenticator : public IAuthenticator {
public:
    AuthOutcome authenticate(const AuthRequest& request) override;
    std::vector<std::string> posture_warnings() const override;
};

/// Seam for phase 2 (token store): when set, `mcp --http` (without --insecure-no-auth) builds its
/// authenticator by calling this; when empty or when it returns null, DenyAllAuthenticator is used.
/// Assign it before run_mcp is called (e.g. from cli_entry.cpp or a function run_mcp calls first).
extern std::function<std::unique_ptr<IAuthenticator>()> g_make_authenticator;

/// Selects the authenticator for an http run: insecure -> AllowAll, else g_make_authenticator(), else DenyAll.
std::unique_ptr<IAuthenticator> make_http_authenticator(bool insecure_no_auth);

} // namespace fairyfly::mcp
