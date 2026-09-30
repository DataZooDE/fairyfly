#include "include/system/powershell_runner.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "include/base64.h"

namespace fairyfly::sys {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string first_line(const std::string& s) {
    std::string line = s.substr(0, s.find_first_of("\r\n"));
    if (line.size() > 300) line.resize(300);
    return line;
}

class WindowsPowerShellRunner final : public PowerShellRunner {
public:
    PsResult run(const std::string& script, const std::string& params_json, int timeout_ms) override {
        PsResult result;
        const std::wstring wide = widen(script);
        std::vector<uint8_t> bytes(wide.size() * 2);
        std::memcpy(bytes.data(), wide.data(), bytes.size());
        const std::wstring encoded = widen(utils::base64_encode(bytes));

        wchar_t sysdir[MAX_PATH] = {};
        GetSystemDirectoryW(sysdir, MAX_PATH);
        const std::wstring exe = std::wstring(sysdir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        std::wstring cmd = L"\"" + exe + L"\" -NoLogo -NoProfile -NonInteractive -OutputFormat Text -ExecutionPolicy Bypass -EncodedCommand " + encoded;

        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
        if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
            throw PowerShellError{"PS_SCRIPT_FAILED", "could not create pipes for PowerShell"};
        }
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = out_w;
        si.hStdError = err_w;
        PROCESS_INFORMATION pi{};

        const std::wstring env_name = widen(kParamsEnvVar);
        SetEnvironmentVariableW(env_name.c_str(), widen(params_json).c_str());
        const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        SetEnvironmentVariableW(env_name.c_str(), nullptr);
        CloseHandle(out_w);
        CloseHandle(err_w);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!created) {
            CloseHandle(out_r);
            CloseHandle(err_r);
            throw PowerShellError{"PS_SCRIPT_FAILED", "could not start powershell.exe"};
        }

        auto drain = [](HANDLE h, std::string& into) {
            DWORD avail = 0;
            while (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                char buf[4096];
                DWORD got = 0;
                if (!ReadFile(h, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || got == 0) break;
                into.append(buf, got);
            }
        };
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
        for (;;) {
            drain(out_r, result.out);
            drain(err_r, result.err);
            if (WaitForSingleObject(pi.hProcess, 25) == WAIT_OBJECT_0) break;
            if (GetTickCount64() > deadline) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 2000);
                result.timed_out = true;
                break;
            }
        }
        drain(out_r, result.out);
        drain(err_r, result.err);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exit_code = static_cast<int>(code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(out_r);
        CloseHandle(err_r);
        return result;
    }
};

} // namespace

std::unique_ptr<PowerShellRunner> make_windows_powershell_runner() { return std::make_unique<WindowsPowerShellRunner>(); }

nlohmann::json run_json_script(PowerShellRunner& runner, const std::string& script, const std::string& params_json, int timeout_ms) {
    const PsResult r = runner.run(script, params_json, timeout_ms);
    if (r.timed_out) throw PowerShellError{"PS_SCRIPT_TIMEOUT", "PowerShell did not finish within " + std::to_string(timeout_ms / 1000) + " s"};
    if (r.exit_code != 0) throw PowerShellError{"PS_SCRIPT_FAILED", "PowerShell failed: " + first_line(r.err)};
    nlohmann::json parsed = nlohmann::json::parse(r.out, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) throw PowerShellError{"PS_SCRIPT_FAILED", "PowerShell returned no JSON result"};
    return parsed;
}

} // namespace fairyfly::sys
