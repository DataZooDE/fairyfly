#include "include/auth/authenticator.h"

#include <algorithm>
#include <cctype>

#include <spdlog/spdlog.h>

#include "include/auth/crypto.h"
#include "include/auth/ip.h"

namespace fairyfly::auth {

namespace {

using mcp::AuthOutcome;
using mcp::AuthRequest;

constexpr const char* kProxyHeaderName = "X-Fairyfly-Proxy-Secret";

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

AuthOutcome deny(int status, const char* code, const std::string& message, const std::string& challenge = {}) {
    AuthOutcome out;
    out.ok = false;
    out.http_status = status;
    out.error_code = code;
    out.message = message;
    out.www_authenticate = challenge;
    return out;
}

const char* kChallenge = "Bearer realm=\"fairyfly\"";
const char* kChallengeInvalid = "Bearer realm=\"fairyfly\", error=\"invalid_token\"";

/// "Bearer <token>" (scheme case-insensitive). nullopt when the header has another shape.
std::optional<std::string> bearer_token(const std::string& header) {
    const std::string h = trim(header);
    if (h.size() < 7) return std::nullopt;
    std::string scheme = h.substr(0, 6);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (scheme != "bearer" || !std::isspace(static_cast<unsigned char>(h[6]))) return std::nullopt;
    const std::string token = trim(h.substr(7));
    if (token.empty() || token.find_first_of(" \t") != std::string::npos) return std::nullopt;
    return token;
}

} // namespace

TokenAuthenticator::TokenAuthenticator(const AuthConfig& config) : config_(config) {
    auto token_backend = config_.token_backend ? config_.token_backend : make_credential_manager_backend(config_.token_prefix);
    proxy_backend_ = config_.proxy_backend ? config_.proxy_backend : make_credential_manager_backend(config_.proxy_prefix);
    tokens_ = std::make_shared<TokenStore>(std::move(token_backend), config_.clock, RandomFn{}, config_.cache_ttl);
}

void TokenAuthenticator::invalidate() {
    tokens_->invalidate();
    std::lock_guard<std::mutex> lock(mutex_);
    proxy_valid_ = false;
}

std::optional<std::string> TokenAuthenticator::proxy_secret() const {
    if (!config_.proxy_secret.empty()) return config_.proxy_secret;
    std::lock_guard<std::mutex> lock(mutex_);
    const TimePoint now = config_.clock ? config_.clock() : std::chrono::system_clock::now();
    if (!proxy_valid_ || now - proxy_loaded_ >= config_.cache_ttl || now < proxy_loaded_) {
        try {
            proxy_cache_ = proxy_backend_->get(config_.proxy_name);
        } catch (const std::exception&) {
            proxy_cache_.reset();  // unreadable => not configured => forwarded headers are ignored (fail closed)
        }
        if (proxy_cache_ && proxy_cache_->empty()) proxy_cache_.reset();
        proxy_loaded_ = now;
        proxy_valid_ = true;
    }
    return proxy_cache_;
}

TokenAuthenticator::ClientAddress TokenAuthenticator::resolve_client(const AuthRequest& request) const {
    ClientAddress out;
    out.address = request.peer_addr;
    const bool has_forwarded = !request.forwarded_for.empty() || !request.forwarded_proto.empty();
    const auto secret = proxy_secret();
    out.proxy_trusted = secret && !request.proxy_secret.empty() && constant_time_equal(*secret, request.proxy_secret);
    if (out.proxy_trusted) {
        if (!request.forwarded_for.empty()) {
            // The trusted proxy appends the address it saw as the LAST entry; earlier entries are client-supplied.
            const std::string& xff = request.forwarded_for;
            const auto comma = xff.find_last_of(',');
            const std::string last = trim(comma == std::string::npos ? xff : xff.substr(comma + 1));
            if (parse_ip(last)) out.address = last;
        }
    } else if (has_forwarded && !warned_forwarded_.exchange(true)) {
        try {
            spdlog::warn("X-Forwarded-* headers received without a valid {}; ignoring them and using the peer address "
                         "(configure the proxy secret to trust the reverse proxy)", kProxyHeaderName);
        } catch (...) {}
    }
    return out;
}

AuthOutcome TokenAuthenticator::authenticate(const AuthRequest& request) {
    const ClientAddress client = resolve_client(request);

    if (config_.insecure_no_auth) {
        AuthOutcome ok;
        ok.ok = true;
        ok.http_status = 200;
        ok.principal.name = "insecure";
        ok.principal.all_scopes = true;
        ok.principal.authenticated = false;
        ok.principal.remote_addr = client.address;
        return ok;
    }

    std::size_t token_count = 0;
    try {
        token_count = tokens_->count();
    } catch (const std::exception&) {
        return deny(503, "AUTH_UNAVAILABLE", "the token store is unavailable");
    }
    if (token_count == 0)
        return deny(401, "AUTH_REQUIRED",
                    "authentication is required and no access tokens are configured on this server; "
                    "create a token: fairyfly mcp token create <name>", kChallenge);

    if (trim(request.authorization).empty())
        return deny(401, "AUTH_REQUIRED", "authentication required: send an Authorization: Bearer <token> header", kChallenge);

    // One message for malformed, unknown-id and wrong-secret tokens: nothing to distinguish them by.
    const auto invalid = [] {
        return deny(401, "TOKEN_INVALID", "the access token is invalid", kChallengeInvalid);
    };

    const auto bearer = bearer_token(request.authorization);
    if (!bearer) return invalid();
    const auto parsed = parse_token(*bearer);
    if (!parsed) return invalid();

    std::optional<TokenMeta> meta;
    try {
        meta = tokens_->find_by_id(parsed->id);
    } catch (const std::exception&) {
        return deny(503, "AUTH_UNAVAILABLE", "the token store is unavailable");
    }
    // Hash even for an unknown id so both outcomes cost the same.
    const std::string presented = sha256_hex(parsed->secret);
    const std::string expected = meta ? meta->secret_hash : std::string(64, '0');
    const bool secret_ok = constant_time_equal(presented, expected);
    if (!meta || !secret_ok) return invalid();

    if (meta->revoked) return deny(401, "TOKEN_REVOKED", "this access token has been revoked", kChallengeInvalid);
    if (meta->expires && tokens_->now() >= *meta->expires)
        return deny(401, "TOKEN_EXPIRED", "this access token has expired", kChallengeInvalid);
    if (!meta->allowed_ips.empty() && !ip_allowed(client.address, meta->allowed_ips))
        return deny(403, "IP_NOT_ALLOWED", "this access token may not be used from this address");

    AuthOutcome ok;
    ok.ok = true;
    ok.http_status = 200;
    ok.principal.id = meta->id;
    ok.principal.name = meta->name;
    ok.principal.all_scopes = meta->has_all_scopes();
    for (const auto& scope : meta->scopes)
        if (scope != "*") ok.principal.scopes.insert(scope);
    ok.principal.sap_systems = meta->sap_systems;
    ok.principal.tcodes = meta->tcodes;
    ok.principal.connections = meta->connections;
    ok.principal.rate_per_minute = meta->rate_per_minute;
    ok.principal.rate_families = meta->rate_families;
    ok.principal.read_only = meta->read_only;
    ok.principal.allow_navigation = meta->allow_navigation;
    ok.principal.remote_addr = client.address;
    ok.principal.authenticated = true;
    return ok;
}

std::vector<std::string> TokenAuthenticator::posture_warnings() const {
    std::vector<std::string> warnings;
    if (config_.insecure_no_auth)
        warnings.push_back("AUTHENTICATION DISABLED (--insecure-no-auth): every client can drive SAP");
    try {
        const auto tokens = tokens_->list();
        std::size_t active = 0;
        std::vector<std::string> no_expiry, all_scopes;
        for (const auto& t : tokens) {
            if (t.revoked) continue;
            ++active;
            if (!t.expires) no_expiry.push_back(t.name);
            if (t.has_all_scopes()) all_scopes.push_back(t.name);
        }
        const auto join = [](const std::vector<std::string>& v) {
            std::string s;
            for (const auto& n : v) s += (s.empty() ? "" : ", ") + n;
            return s;
        };
        if (active == 0)
            warnings.push_back("no access tokens configured: the server answers 401 to every request; "
                               "create a token: fairyfly mcp token create <name>");
        if (!no_expiry.empty()) warnings.push_back("tokens without an expiry: " + join(no_expiry));
        if (!all_scopes.empty()) warnings.push_back("tokens with all scopes (*): " + join(all_scopes));
    } catch (const std::exception&) {
        warnings.push_back("the token store could not be read: every request will fail");
    }
    if (!proxy_secret())
        warnings.push_back("proxy secret not set: X-Forwarded-* headers are ignored, so behind a reverse proxy every client appears as the "
                           "proxy address and token IP binding cannot work");
    return warnings;
}

std::unique_ptr<mcp::IAuthenticator> make_default_authenticator(const AuthConfig& config) {
    return std::make_unique<TokenAuthenticator>(config);
}

} // namespace fairyfly::auth
