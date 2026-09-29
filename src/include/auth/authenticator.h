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
    /// Credential Manager entry of the proxy secret: "<proxy_prefix><proxy_name>" = "fairyfly-mcp-proxy".
    std::string proxy_prefix = "fairyfly-mcp-";
    std::string proxy_name = "proxy";
    /// Explicit proxy secret (highest priority; e.g. from the tray/config). Empty = read it from the backend.
    std::string proxy_secret;
    /// Backends; null = the Windows Credential Manager. Tests inject InMemorySecretBackend.
    std::shared_ptr<SecretBackend> token_backend;
    std::shared_ptr<SecretBackend> proxy_backend;
    Clock clock;                                              ///< null = system clock
    std::chrono::milliseconds cache_ttl{5000};                ///< max age of cached token/proxy records (revoke latency)
    /// `mcp --insecure-no-auth`: accept every request as an unauthenticated, all-scope principal (loud warning).
    bool insecure_no_auth = false;
};

class TokenAuthenticator final : public mcp::IAuthenticator {
public:
    explicit TokenAuthenticator(const AuthConfig& config);
    mcp::AuthOutcome authenticate(const mcp::AuthRequest& request) override;
    std::vector<std::string> posture_warnings() const override;

    TokenStore& tokens() { return *tokens_; }
    /// Drops cached token and proxy-secret records (revoke/rotate elsewhere become visible at once).
    void invalidate();

    /// Trusted-proxy resolution (public for tests): the client address for this request, and whether the
    /// forwarded headers were honoured.
    struct ClientAddress { std::string address; bool proxy_trusted = false; };
    ClientAddress resolve_client(const mcp::AuthRequest& request) const;

private:
    std::optional<std::string> proxy_secret() const;

    AuthConfig config_;
    std::shared_ptr<TokenStore> tokens_;
    std::shared_ptr<SecretBackend> proxy_backend_;
    mutable std::mutex mutex_;
    mutable std::optional<std::string> proxy_cache_;
    mutable TimePoint proxy_loaded_{};
    mutable bool proxy_valid_ = false;
    mutable std::atomic<bool> warned_forwarded_{false};
};

/// The seam for phase 1: builds the token authenticator over the Credential Manager (or the injected backends).
std::unique_ptr<mcp::IAuthenticator> make_default_authenticator(const AuthConfig& config);

} // namespace fairyfly::auth
