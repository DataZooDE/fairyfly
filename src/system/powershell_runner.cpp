#include "include/system/powershell_runner.h"
#include "include/system/unique_handle.h"

#include "include/text/utf8.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "include/base64.h"

namespace fairyfly::sys {

namespace {

std::string first_line(const std::string& s) {
    std::string line = s.substr(0, s.find_first_of("\r\n"));
    if (line.size() > 300) line.resize(300);
    return line;
}

class WindowsPowerShellRunner final : public PowerShellRunner {
public:
    PsResult run(const std::string& script, const std::string& params_json, int timeout_ms) override {
        PsResult result;
        const std::wstring wide = text::utf8_to_wide(script);
        std::vector<uint8_t> bytes(wide.size() * 2);
        std::memcpy(bytes.data(), wide.data(), bytes.size());
        const std::wstring encoded = text::utf8_to_wide(utils::base64_encode(bytes));

        wchar_t sysdir[MAX_PATH] = {};
        GetSystemDirectoryW(sysdir, MAX_PATH);
        const std::wstring exe = std::wstring(sysdir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        std::wstring cmd = L"\"" + exe + L"\" -NoLogo -NoProfile -NonInteractive -OutputFormat Text -ExecutionPolicy Bypass -EncodedCommand " + encoded;

        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        system::UniqueHandle out_r, out_w, err_r, err_w;   // every return and throw below closes what is open
        if (!CreatePipe(out_r.out(), out_w.out(), &sa, 0) || !CreatePipe(err_r.out(), err_w.out(), &sa, 0))
            throw PowerShellError{"PS_SCRIPT_FAILED", "could not create pipes for PowerShell"};
        if (!SetHandleInformation(out_r.get(), HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(err_r.get(), HANDLE_FLAG_INHERIT, 0))
            throw PowerShellError{"PS_SCRIPT_FAILED", "could not restrict PowerShell pipe handles"};
        system::UniqueHandle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
        if (!nul) throw PowerShellError{"PS_SCRIPT_FAILED", "could not open PowerShell input sink"};

        SIZE_T attribute_bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        std::vector<unsigned char> attribute_storage(attribute_bytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes))
            throw PowerShellError{"PS_SCRIPT_FAILED", "could not restrict PowerShell handles"};
        struct AttributeGuard {
            LPPROC_THREAD_ATTRIBUTE_LIST value;
            ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
        } attribute_guard{attributes};
        std::array<HANDLE, 3> inherited{nul.get(), out_w.get(), err_w.get()};   // the child inherits only these
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inherited.data(), sizeof inherited, nullptr, nullptr))
            throw PowerShellError{"PS_SCRIPT_FAILED", "could not restrict PowerShell handles"};
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul.get();
        si.StartupInfo.hStdOutput = out_w.get();
        si.StartupInfo.hStdError = err_w.get();
        si.lpAttributeList = attributes;
        PROCESS_INFORMATION pi{};

        config::set_process_env(kParamsEnvVar, params_json);
        const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                            nullptr, nullptr, &si.StartupInfo, &pi);
        config::unset_process_env(kParamsEnvVar);
        // the child holds its own copies; ours would keep the pipes open after it exits
        out_w.reset();
        err_w.reset();
        nul.reset();
        if (!created) throw PowerShellError{"PS_SCRIPT_FAILED", "could not start powershell.exe"};
        const system::UniqueHandle process(pi.hProcess), thread(pi.hThread);

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
            drain(out_r.get(), result.out);
            drain(err_r.get(), result.err);
            if (WaitForSingleObject(process.get(), 25) == WAIT_OBJECT_0) break;
            if (GetTickCount64() > deadline) {
                TerminateProcess(process.get(), 1);
                WaitForSingleObject(process.get(), 2000);
                result.timed_out = true;
                break;
            }
        }
        drain(out_r.get(), result.out);
        drain(err_r.get(), result.err);
        DWORD code = 0;
        GetExitCodeProcess(process.get(), &code);
        result.exit_code = static_cast<int>(code);
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
