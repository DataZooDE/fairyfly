#include "include/system/window_owner.h"

#ifdef _WIN32
#include <windows.h>

#include <vector>

namespace fairyfly::system {
namespace {

struct ScopedHandle {
    HANDLE value = nullptr;
    ~ScopedHandle() { if (value) CloseHandle(value); }
};

std::vector<unsigned char> token_information(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    (void)GetTokenInformation(token, kind, nullptr, 0, &size);
    if (!size) return {};
    std::vector<unsigned char> bytes(size);
    if (!GetTokenInformation(token, kind, bytes.data(), size, &size)) return {};
    return bytes;
}

bool same_logon(HANDLE target_process, std::uint32_t target_pid) {
    DWORD own_session = 0, target_session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &own_session) ||
        !ProcessIdToSessionId(target_pid, &target_session) || own_session != target_session)
        return false;
    ScopedHandle own_token, target_token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own_token.value) ||
        !OpenProcessToken(target_process, TOKEN_QUERY, &target_token.value)) return false;
    const auto own_user = token_information(own_token.value, TokenUser);
    const auto target_user = token_information(target_token.value, TokenUser);
    const auto own_stats = token_information(own_token.value, TokenStatistics);
    const auto target_stats = token_information(target_token.value, TokenStatistics);
    if (own_user.size() < sizeof(TOKEN_USER) || target_user.size() < sizeof(TOKEN_USER) ||
        own_stats.size() < sizeof(TOKEN_STATISTICS) || target_stats.size() < sizeof(TOKEN_STATISTICS)) return false;
    const auto* own = reinterpret_cast<const TOKEN_USER*>(own_user.data());
    const auto* target = reinterpret_cast<const TOKEN_USER*>(target_user.data());
    const auto* own_stat = reinterpret_cast<const TOKEN_STATISTICS*>(own_stats.data());
    const auto* target_stat = reinterpret_cast<const TOKEN_STATISTICS*>(target_stats.data());
    return EqualSid(own->User.Sid, target->User.Sid) != FALSE &&
        own_stat->AuthenticationId.HighPart == target_stat->AuthenticationId.HighPart &&
        own_stat->AuthenticationId.LowPart == target_stat->AuthenticationId.LowPart;
}

} // namespace

bool process_owned_by_current_logon(std::uint32_t process_id) noexcept {
    try {
        if (!process_id) return false;
        ScopedHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id)};
        if (!process.value) return false;
        return same_logon(process.value, process_id);
    } catch (...) { return false; }
}

bool window_owned_by_current_logon(std::uintptr_t native_handle) noexcept {
    try {
        if (!native_handle) return false;
        HWND window = reinterpret_cast<HWND>(native_handle);
        if (!IsWindow(window)) return false;
        DWORD process_id = 0;
        if (!GetWindowThreadProcessId(window, &process_id) || !process_id) return false;
        ScopedHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id)};
        if (!process.value) return false;
        if (!same_logon(process.value, process_id)) return false;
        DWORD after = 0;
        return IsWindow(window) && GetWindowThreadProcessId(window, &after) && after == process_id;
    } catch (...) { return false; }
}

} // namespace fairyfly::system

#else
namespace fairyfly::system {
bool process_owned_by_current_logon(std::uint32_t) noexcept { return false; }
bool window_owned_by_current_logon(std::uintptr_t) noexcept { return false; }
} // namespace fairyfly::system
#endif
