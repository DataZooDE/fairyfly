#pragma once

// Credential storage for SAP GUI logons. The production implementation uses the
// Windows Credential Manager; an in-memory implementation exists for tests.
// Layout: CRED_TYPE_GENERIC, CRED_PERSIST_LOCAL_MACHINE, target "fairyfly:<connection>",
// user name = SAP user, blob = password (UTF-16LE, no NUL), non-secret metadata in attributes.
// Nothing here logs or formats a password.

#include "secret_buffer.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fairyfly::cred {

constexpr size_t kMaxPasswordBlobBytes = 2560;  ///< CRED_MAX_CREDENTIAL_BLOB_SIZE
constexpr std::string_view kTargetPrefix = "fairyfly:";

/// Error with a machine-readable code (e.g. CREDENTIAL_STORE_UNAVAILABLE). The message never contains a secret.
class CredentialError : public std::runtime_error {
public:
    CredentialError(std::string code, const std::string& message)
        : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const { return code_; }

private:
    std::string code_;
};

/// Non-secret description of a stored credential.
struct CredentialSummary {
    std::string connection;
    std::string username;
    std::string client;
    std::string language;
    std::string updated_at;  ///< ISO 8601 UTC, from LastWritten

    nlohmann::json to_json() const;  ///< never contains a secret
};

struct StoredCredential {
    CredentialSummary meta;
    SecretBuffer password;
};

class CredentialStore {
public:
    virtual ~CredentialStore() = default;
    /// nullopt when no credential is stored under that connection name.
    virtual std::optional<StoredCredential> read(const std::string& connection) = 0;
    /// Create or replace. Throws CredentialError on failure.
    virtual void write(const std::string& connection, const CredentialSummary& meta, const SecretBuffer& password) = 0;
    /// true when something was removed.
    virtual bool remove(const std::string& connection) = 0;
    virtual std::vector<CredentialSummary> list() = 0;
};

// Pure helpers (unit-testable without the Windows store).
std::string trim_connection_name(std::string_view connection);
/// "fairyfly:" + trimmed connection name. Throws CredentialError(INVALID_CONNECTION_NAME) when empty/invalid.
std::wstring target_name(const std::string& connection);
/// Inverse of target_name; nullopt for foreign or empty targets.
std::optional<std::string> connection_from_target(std::wstring_view target);
/// Overloads with an explicit target prefix (additive; the remote-MCP token store uses "fairyfly-mcp:").
std::wstring target_name(const std::string& connection, std::wstring_view prefix);
std::optional<std::string> connection_from_target(std::wstring_view target, std::wstring_view prefix);
/// UTF-8 password -> UTF-16LE bytes without terminating NUL. Throws CredentialError(PASSWORD_TOO_LONG) above 2560 bytes.
std::vector<unsigned char> encode_password_blob(const std::string& utf8);
SecretBuffer decode_password_blob(const unsigned char* data, size_t size);

class WindowsCredentialStore final : public CredentialStore {
public:
    /// `target_prefix` (UTF-8) defaults to "fairyfly:"; the token store passes "fairyfly-mcp:".
    explicit WindowsCredentialStore(std::string target_prefix = std::string(kTargetPrefix));
    std::optional<StoredCredential> read(const std::string& connection) override;
    void write(const std::string& connection, const CredentialSummary& meta, const SecretBuffer& password) override;
    bool remove(const std::string& connection) override;
    std::vector<CredentialSummary> list() override;

private:
    std::wstring prefix_;
};

class InMemoryCredentialStore final : public CredentialStore {
public:
    std::optional<StoredCredential> read(const std::string& connection) override;
    void write(const std::string& connection, const CredentialSummary& meta, const SecretBuffer& password) override;
    bool remove(const std::string& connection) override;
    std::vector<CredentialSummary> list() override;

private:
    struct Entry {
        CredentialSummary meta;
        SecretBuffer password;
    };
    std::map<std::string, Entry> entries_;
};

std::unique_ptr<CredentialStore> make_default_store();

} // namespace fairyfly::cred
