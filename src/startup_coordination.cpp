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
bool LoginAttempts::clear(const std::string& connection) {
    if (root_.empty() || !root_.is_absolute() || connection.empty()) return false;
    try {
        std::filesystem::remove(attempt_path(root_, connection));
        return true;
    } catch (...) { return false; }
}
} // namespace fairyfly::startup
