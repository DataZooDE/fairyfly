#include "include/startup_coordination.h"
#include "include/auth/crypto.h"
#include "include/string_utils.h"
#include <algorithm>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fairyfly::startup {
namespace {
struct NativeGuard {
#ifdef _WIN32
    HANDLE handle = nullptr;
    bool held = false;
    ~NativeGuard() { if (held) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
#else
    static std::timed_mutex mutex;
    bool held = false;
    ~NativeGuard() { if (held) mutex.unlock(); }
#endif
};
#ifndef _WIN32
std::timed_mutex NativeGuard::mutex;
#endif
std::filesystem::path attempt_path(const std::filesystem::path& root, const std::string& connection) {
    return root / "startup-login-attempts" / auth::sha256_hex(utils::lower_ascii(connection));
}
}

PreparationGuard acquire_preparation_guard(const std::function<bool()>& cancelled, std::chrono::milliseconds timeout) {
    auto guard = std::make_shared<NativeGuard>();
#ifdef _WIN32
    guard->handle = CreateMutexW(nullptr, FALSE, L"Local\\fairyfly.sap-session-prepare");
    if (!guard->handle)
        return {Result::fail("SAP_STARTUP_LOCK_FAILED", "Cannot coordinate SAP session startup; retry the task."), {}};
#endif
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (cancelled && cancelled())
            return {Result::fail("CANCELLED", "SAP session preparation was cancelled."), {}};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        const auto slice = std::clamp(remaining, std::chrono::milliseconds(0), std::chrono::milliseconds(50));
#ifdef _WIN32
        const auto wait = WaitForSingleObject(guard->handle, static_cast<DWORD>(slice.count()));
        guard->held = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (wait == WAIT_FAILED)
            return {Result::fail("SAP_STARTUP_LOCK_FAILED", "Cannot wait for SAP session startup."), {}};
#else
        guard->held = NativeGuard::mutex.try_lock_for(slice);
#endif
        if (guard->held) return {Result::ok(), std::move(guard)};
        if (std::chrono::steady_clock::now() >= deadline)
            return {Result::fail("SAP_STARTUP_BUSY", "Another Fairyfly process is preparing SAP; wait briefly and retry."), {}};
    }
}

std::string LoginAttempts::credential_key(const std::string& identity) {
    const auto first = identity.find('/');
    const auto second = first == std::string::npos ? first : identity.find('/', first + 1);
    if (first == 0 || first == std::string::npos || second == std::string::npos || second == first + 1 ||
        second + 1 == identity.size() || identity.find('/', second + 1) != std::string::npos) return {};
    // Account lockout counts across callers, connection aliases and transports.
    return "sap-credential-identity\n" + identity;
}
bool LoginAttempts::has_login_attempts() const {
    if (root_.empty() || !root_.is_absolute()) return false;
    std::error_code error;
    const auto path = root_ / "startup-login-attempts";
    const std::filesystem::directory_iterator entries(path, error);
    if (error) return error != std::errc::no_such_file_or_directory;
    return entries != std::filesystem::directory_iterator{};
}

Result LoginAttempts::begin(const std::string& connection) {
    if (root_.empty() || !root_.is_absolute() || connection.empty())
        return Result::fail("SAP_LOGIN_GUARD_UNAVAILABLE", "Automatic login cannot safely record its attempt. Log in to SAP manually.");
    try {
        const auto path = attempt_path(root_, connection);
        std::filesystem::create_directories(path.parent_path());
        if (!std::filesystem::create_directory(path))
            return Result::fail("SAP_LOGIN_REQUIRED", "An earlier automatic SAP login needs manual attention; it will not be retried automatically.",
                json::array({"Complete login or SSO in SAP GUI, then retry the original task. If credentials changed, log in manually once to confirm them."}));
        return Result::ok();
    } catch (...) {
        return Result::fail("SAP_LOGIN_GUARD_UNAVAILABLE", "Automatic login cannot safely record its attempt. Log in to SAP manually.");
    }
}
Result LoginAttempts::begin_launch(const std::string& connection) {
    const auto result = LoginAttempts(root_ / "startup-launch-attempts").begin(connection);
    if (result.status == Result::Status::Success) return result;
    return Result::fail("SAP_STARTUP_REQUIRES_ATTENTION",
        "An earlier automatic SAP launch is incomplete or cannot be recorded safely; no additional window was opened.",
        json::array({"Inspect SAP Logon and the local default connection. Start the intended session and log in manually, then retry the original task."}));
}
bool LoginAttempts::pending(const std::string& connection) const {
    if (root_.empty() || !root_.is_absolute() || connection.empty()) return false;
    std::error_code error;
    const bool login = std::filesystem::exists(attempt_path(root_, connection), error) && !error;
    error.clear();
    const bool launch = std::filesystem::exists(attempt_path(root_ / "startup-launch-attempts", connection), error) && !error;
    return login || launch;
}
bool LoginAttempts::clear(const std::string& connection) {
    if (root_.empty() || !root_.is_absolute() || connection.empty()) return false;
    try {
        std::error_code login_error, launch_error;
        std::filesystem::remove(attempt_path(root_, connection), login_error);
        std::filesystem::remove(attempt_path(root_ / "startup-launch-attempts", connection), launch_error);
        return !login_error && !launch_error;
    } catch (...) { return false; }
}
} // namespace fairyfly::startup
