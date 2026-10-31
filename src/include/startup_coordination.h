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
    bool has_login_attempts() const;
    Result begin(const std::string& connection);
    // Separate durable launch guard: a failed/uncertain launch must not create
    // another invisible window on every task. Verified login clears both.
    Result begin_launch(const std::string& connection);
    bool pending(const std::string& connection) const;
    bool clear(const std::string& connection);
private:
    std::filesystem::path root_;
};
} // namespace fairyfly::startup
