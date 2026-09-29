#include "include/auth/secret_backend.h"

namespace fairyfly::auth {

namespace {
constexpr const char* kBackendUser = "fairyfly-mcp";
}

std::optional<std::string> CredentialStoreBackend::get(const std::string& name) {
    try {
        auto stored = store_->read(name);
        if (!stored) return std::nullopt;
        return std::string(stored->password.utf8());
    } catch (const cred::CredentialError& e) {
        throw AuthError("TOKEN_STORE_UNAVAILABLE", e.what());
    }
}

void CredentialStoreBackend::put(const std::string& name, const std::string& value) {
    try {
        cred::CredentialSummary meta;
        meta.connection = name;
        meta.username = kBackendUser;
        store_->write(name, meta, cred::SecretBuffer(std::string(value)));
    } catch (const cred::CredentialError& e) {
        throw AuthError("TOKEN_STORE_UNAVAILABLE", e.what());
    }
}

bool CredentialStoreBackend::remove(const std::string& name) {
    try {
        return store_->remove(name);
    } catch (const cred::CredentialError& e) {
        throw AuthError("TOKEN_STORE_UNAVAILABLE", e.what());
    }
}

std::vector<std::string> CredentialStoreBackend::list_names() {
    try {
        std::vector<std::string> names;
        for (const auto& summary : store_->list()) names.push_back(summary.connection);
        return names;
    } catch (const cred::CredentialError& e) {
        throw AuthError("TOKEN_STORE_UNAVAILABLE", e.what());
    }
}

std::optional<std::string> InMemorySecretBackend::get(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(name);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

void InMemorySecretBackend::put(const std::string& name, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[name] = value;
}

bool InMemorySecretBackend::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.erase(name) > 0;
}

std::vector<std::string> InMemorySecretBackend::list_names() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& [name, value] : entries_) names.push_back(name);
    return names;
}

std::size_t InMemorySecretBackend::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::vector<std::string> InMemorySecretBackend::all_values() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> values;
    for (const auto& [name, value] : entries_) values.push_back(value);
    return values;
}

std::shared_ptr<SecretBackend> make_credential_manager_backend(const std::string& target_prefix) {
    return std::make_shared<CredentialStoreBackend>(std::make_shared<cred::WindowsCredentialStore>(target_prefix));
}

} // namespace fairyfly::auth
