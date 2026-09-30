// Elevation state (token), user SIDs and the one-shot UAC self-elevation: `<exe> mcp setup --apply-plan F --result-file R`.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <sddl.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "include/setup/setup_hosts.h"
#include "win_util.h"

namespace fairyfly::setup {

namespace {

using namespace win;

struct TokenHandle {
    HANDLE h = nullptr;
    TokenHandle() {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h)) h = nullptr;
    }
    ~TokenHandle() {
        if (h) CloseHandle(h);
    }
};

std::string sid_to_string(PSID sid) {
    LPWSTR text = nullptr;
    std::string out;
    if (ConvertSidToStringSidW(sid, &text)) {
        out = narrow(text);
        LocalFree(text);
    }
    return out;
}

std::string exe_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) {
            buffer.resize(n);
            return narrow(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::string local_app_data_dir() {
    char* value = nullptr;
    size_t size = 0;
    std::string out;
    if (_dupenv_s(&value, &size, "LOCALAPPDATA") == 0 && value) {
        out = value;
        free(value);
    }
    if (out.empty()) out = std::filesystem::temp_directory_path().string();
    return out;
}

class RealElevator : public Elevator {
public:
    bool is_elevated() override {
        TokenHandle token;
        if (!token.h) return false;
        TOKEN_ELEVATION e{};
        DWORD size = 0;
        return GetTokenInformation(token.h, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated != 0;
    }

    ElevationType elevation_type() override {
        TokenHandle token;
        if (!token.h) return ElevationType::Unknown;
        TOKEN_ELEVATION_TYPE type{};
        DWORD size = 0;
        if (!GetTokenInformation(token.h, TokenElevationType, &type, sizeof(type), &size)) return ElevationType::Unknown;
        if (type == TokenElevationTypeFull) return ElevationType::Elevated;
        if (type == TokenElevationTypeLimited) return ElevationType::AdminFiltered;
        return is_elevated() ? ElevationType::Elevated : ElevationType::StandardUser;   // Default: UAC off or plain user
    }

    std::string current_user_sid() override {
        TokenHandle token;
        if (!token.h) return {};
        DWORD size = 0;
        GetTokenInformation(token.h, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> buffer(size);
        if (!GetTokenInformation(token.h, TokenUser, buffer.data(), size, &size)) return {};
        return sid_to_string(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
    }

    std::string current_user_name() override {
        TokenHandle token;
        if (!token.h) return {};
        DWORD size = 0;
        GetTokenInformation(token.h, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> buffer(size);
        if (!GetTokenInformation(token.h, TokenUser, buffer.data(), size, &size)) return {};
        return account_of(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
    }

    std::optional<std::string> resolve_user_sid(const std::string& user) override {
        const std::wstring name = widen(user);
        DWORD sid_size = 0, domain_size = 0;
        SID_NAME_USE use;
        LookupAccountNameW(nullptr, name.c_str(), nullptr, &sid_size, nullptr, &domain_size, &use);
        if (sid_size == 0) return std::nullopt;
        std::vector<BYTE> sid(sid_size);
        std::wstring domain(domain_size, L'\0');
        if (!LookupAccountNameW(nullptr, name.c_str(), sid.data(), &sid_size, domain.data(), &domain_size, &use)) return std::nullopt;
        const std::string text = sid_to_string(sid.data());
        if (text.empty()) return std::nullopt;
        return text;
    }

    ElevatedRun run_elevated(const nlohmann::json& plan) override {
        ElevatedRun run;
        namespace fs = std::filesystem;
        const fs::path dir = win::to_path(local_app_data_dir()) / "fairyfly" / "run";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::string stem = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
        const fs::path plan_file = dir / ("plan-" + stem + ".json");
        const fs::path result_file = dir / ("result-" + stem + ".json");
        {
            std::ofstream out(plan_file, std::ios::binary | std::ios::trunc);
            out << plan.dump();
            if (!out) {
                run.error = "cannot write the plan file " + plan_file.string();
                return run;
            }
        }
        const std::string exe = exe_path();
        const std::wstring params = L"mcp setup --apply-plan \"" + plan_file.wstring() + L"\" --result-file \"" + result_file.wstring() + L"\"";
        const std::wstring wexe = widen(exe);
        const std::wstring wdir = win::to_path(exe).parent_path().wstring();
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"runas";
        info.lpFile = wexe.c_str();
        info.lpParameters = params.c_str();
        info.lpDirectory = wdir.c_str();
        info.nShow = SW_HIDE;
        const BOOL started = ShellExecuteExW(&info);
        const DWORD start_error = started ? 0 : GetLastError();
        if (SUCCEEDED(com)) CoUninitialize();
        if (!started) {
            if (start_error == ERROR_CANCELLED) run.declined = true;
            else run.error = error_text(start_error);
            fs::remove(plan_file, ec);
            return run;
        }
        run.launched = true;
        if (info.hProcess) {
            const DWORD wait = WaitForSingleObject(info.hProcess, 10 * 60 * 1000);
            DWORD code = 0;
            if (wait == WAIT_TIMEOUT) {
                TerminateProcess(info.hProcess, 1);
                run.error = "the elevated process timed out";
            } else if (GetExitCodeProcess(info.hProcess, &code)) {
                run.exit_code = static_cast<int>(code);
            }
            CloseHandle(info.hProcess);
        }
        std::ifstream in(result_file, std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            try {
                run.result = nlohmann::json::parse(ss.str());
            } catch (const std::exception&) {
                run.error = "unreadable result file";
            }
        }
        fs::remove(plan_file, ec);
        fs::remove(result_file, ec);
        return run;
    }

private:
    static std::string account_of(PSID sid) {
        DWORD name_size = 0, domain_size = 0;
        SID_NAME_USE use;
        LookupAccountSidW(nullptr, sid, nullptr, &name_size, nullptr, &domain_size, &use);
        std::wstring name(name_size, L'\0'), domain(domain_size, L'\0');
        if (!LookupAccountSidW(nullptr, sid, name.data(), &name_size, domain.data(), &domain_size, &use)) return sid_to_string(sid);
        name.resize(name_size);
        domain.resize(domain_size);
        return domain.empty() ? narrow(name) : narrow(domain) + "\\" + narrow(name);
    }
};

} // namespace

std::unique_ptr<Elevator> make_real_elevator() { return std::make_unique<RealElevator>(); }

} // namespace fairyfly::setup
