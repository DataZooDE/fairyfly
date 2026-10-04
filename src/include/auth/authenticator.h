#pragma once
// Bearer-token authentication for the remote MCP endpoint (implements mcp::IAuthenticator).
// This is the seam phase 1 (run_mcp / HTTP endpoint) uses: make_default_authenticator(AuthConfig).

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "include/auth/secret_backend.h"
#include "include/auth/token_store.h"
#include "include/mcp/principal.h"

namespace fairyfly::auth {

struct AuthConfig {
    /// Credential Manager target prefix of the token records ("fairyfly-mcp:<name>").
    std::string token_prefix = kTokenTargetPrefix;
    /// Backends; null = the Windows Credential Manager. Tests inject InMemorySecretBackend.
    std::shared_ptr<SecretBackend> token_backend;
    Clock clock;                                              ///< null = system clock
    std::chrono::milliseconds cache_ttl{5000};                ///< max age of cached token records (revoke latency)
    /// `mcp --insecure-no-auth`: accept every request as an unauthenticated, all-scope principal (loud warning).
    bool insecure_no_auth = false;
};

class TokenAuthenticator final : public mcp::IAuthenticator {
public:
    explicit TokenAuthenticator(const AuthConfig& config);
    mcp::AuthOutcome authenticate(const mcp::AuthRequest& request) override;
    std::vector<std::string> posture_warnings() const override;

    TokenStore& tokens() { return *tokens_; }
    /// Drops cached token records (revoke/rotate elsewhere become visible at once).
    void invalidate();
    void invalidate_cache() override { invalidate(); }

private:
    AuthConfig config_;
    std::shared_ptr<TokenStore> tokens_;
};

/// The seam for phase 1: builds the token authenticator over the Credential Manager (or the injected backends).
std::unique_ptr<mcp::IAuthenticator> make_default_authenticator(const AuthConfig& config);

} // namespace fairyfly::auth
