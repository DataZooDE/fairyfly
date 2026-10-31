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
// This state contains only a hash of the locally configured connection name.
class LoginAttempts {
public:
    explicit LoginAttempts(std::filesystem::path root) : root_(std::move(root)) {}
    Result begin(const std::string& connection);
    bool clear(const std::string& connection);
private:
    std::filesystem::path root_;
};
} // namespace fairyfly::startup
