#pragma once
// Where token records (and the proxy secret) live. The real backend is the Windows Credential
// Manager through cred::CredentialStore with a configurable target prefix; tests use the in-memory one.

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "include/credential_store.h"

namespace fairyfly::auth {

/// Error with a machine-readable code (UNKNOWN_FAMILY, TOKEN_EXISTS, ...). Messages never hold secrets.
class AuthError : public std::runtime_error {
public:
    AuthError(std::string code, const std::string& message) : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const { return code_; }
private:
    std::string code_;
};

/// A named string store. Values are small (a token JSON blob or the proxy secret).
class SecretBackend {
public:
    virtual ~SecretBackend() = default;
    virtual std::optional<std::string> get(const std::string& name) = 0;
    virtual void put(const std::string& name, const std::string& value) = 0;
    virtual bool remove(const std::string& name) = 0;
    virtual std::vector<std::string> list_names() = 0;
    /// Largest value the backend can hold (Credential Manager: 1280 UTF-16 characters).
    virtual std::size_t max_value_chars() const { return 1200; }
};

/// Adapter over any cred::CredentialStore (the value is stored as the credential's "password").
class CredentialStoreBackend final : public SecretBackend {
public:
    explicit CredentialStoreBackend(std::shared_ptr<cred::CredentialStore> store) : store_(std::move(store)) {}
    std::optional<std::string> get(const std::string& name) override;
    void put(const std::string& name, const std::string& value) override;
    bool remove(const std::string& name) override;
    std::vector<std::string> list_names() override;
private:
    std::shared_ptr<cred::CredentialStore> store_;
};

/// Test double; thread-safe.
class InMemorySecretBackend final : public SecretBackend {
public:
    std::optional<std::string> get(const std::string& name) override;
    void put(const std::string& name, const std::string& value) override;
    bool remove(const std::string& name) override;
    std::vector<std::string> list_names() override;
    std::size_t size() const;
    /// Every stored value, for tests that assert no secret is stored.
    std::vector<std::string> all_values() const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> entries_;
};

/// Windows Credential Manager backend: targets "<target_prefix><name>", e.g. "fairyfly-mcp:ci-bot".
std::shared_ptr<SecretBackend> make_credential_manager_backend(const std::string& target_prefix);

} // namespace fairyfly::auth
