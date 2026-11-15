#pragma once
#include "core.h"
#include <filesystem>
#include <functional>

namespace fairyfly::startup {
struct PreparationGuard {
    Result result;
    std::shared_ptr<void> token;
};
PreparationGuard acquire_preparation_guard(const std::function<bool()>& cancelled = {},
    std::chrono::milliseconds timeout = std::chrono::seconds(20));

// An attempt is recorded BEFORE submitting credentials. Failure, cancellation,
// process death and uncertain outcomes all require manual login before retry.
// State contains hashes of launch scopes or SAP identities, never credentials.
class LoginAttempts {
public:
    explicit LoginAttempts(std::filesystem::path root) : root_(std::move(root)) {}
    static std::string credential_key(const std::string& identity);
    /// The first empty part of a "SYS/CLIENT/USER" identity ("system", "client" or "user"); empty when
    /// all three are present, "identity" when the shape itself is wrong.
    static std::string missing_identity_part(const std::string& identity);
    bool has_login_attempts() const;
    Result begin(const std::string& connection);
    /// begin(credential_key(identity)); logs which identity part is missing (never its values) when it refuses.
    Result begin_login(const std::string& identity);
    // Separate durable launch guard: a failed/uncertain launch must not create
    // another invisible window on every task. Verified login clears both.
    Result begin_launch(const std::string& connection);
    bool pending(const std::string& connection) const;
    bool clear(const std::string& connection);
private:
    std::filesystem::path root_;
};
} // namespace fairyfly::startup
