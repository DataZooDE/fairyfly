#pragma once

#include "core.h"
#include "credential_store.h"
#include "login_flow.h"

#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fairyfly::cred {

/// Where `login` may take its credentials from.
struct LoginSource {
    std::optional<std::string> credentials_file;
    bool from_stdin = false;
    std::optional<std::string> credential_name;
};

struct ResolvedLogin {
    LoginCredentials credentials;
    std::string source;  ///< "stdin", "file" or "credential-manager"
    std::vector<std::string> warnings;
};

using OpenFileFn = std::function<std::unique_ptr<std::istream>(const std::string& path)>;

/// Order: stdin, credentials file (deprecated), credential store (credential_name or
/// default_connection). There is deliberately no implicit ./trial.env fallback.
/// Errors: CREDENTIALS_NOT_FOUND, CREDENTIAL_FILE_UNAVAILABLE, INVALID_CREDENTIAL_FILE,
/// CREDENTIAL_STORE_UNAVAILABLE, CREDENTIAL_STORE_ERROR.
ResultT<ResolvedLogin> resolve_login_credentials(const LoginSource& source,
                                                 const std::string& default_connection,
                                                 CredentialStore& store,
                                                 std::istream& stdin_stream,
                                                 const OpenFileFn& open_file);

/// Zero every secret string held by the credentials.
void scrub_login_credentials(LoginCredentials& credentials) noexcept;

} // namespace fairyfly::cred
