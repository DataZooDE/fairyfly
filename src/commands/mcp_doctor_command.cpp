#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wtsapi32.h>
#include <wchar.h>

#include <iostream>

#include <spdlog/spdlog.h>

#include "include/auth/secret_backend.h"
#include "include/auth/token_store.h"
#include "include/commands/mcp_extras.h"
#include "include/config/mcp_doctor.h"
#include "include/tray/tray_win32.h"

namespace fairyfly {
namespace commands {

using namespace fairyfly::config;

namespace {

/// Real probes. SAP state comes from the existing read-only `doctor` handler; nothing is clicked.
class RealProbes : public DoctorProbes {
public:
    explicit RealProbes(const HandlerProvider& get_handler) : get_handler_(get_handler) {}

    PortState probe_port(const std::string& host, int port) override {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return PortState::Unknown;
        PortState state = PortState::Unknown;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        const std::string ip = (host == "localhost" || host.empty()) ? "127.0.0.1" : host;
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) == 1) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s != INVALID_SOCKET) {
                const BOOL exclusive = TRUE;
                setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
                if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) state = PortState::Free;
                else if (WSAGetLastError() == WSAEADDRINUSE || WSAGetLastError() == WSAEACCES) state = PortState::InUse;
                closesocket(s);
            }
        }
        WSACleanup();
        return state;
    }

    SapState sap() override {
        SapState out;
        try {
            const Result result = get_handler_().handle_doctor();
            if (result.status != Result::Status::Success || !result.data.contains("checks")) return out;
            for (const auto& c : result.data["checks"]) {
                const std::string name = c.value("name", "");
                const std::string status = c.value("status", "");
                if (name == "com_engine")
                    out.scripting_available = status == "pass" ? SapState::Tri::Yes : SapState::Tri::No;
                if (name == "active_sessions") {
                    out.session_present = status == "pass" ? SapState::Tri::Yes : SapState::Tri::No;
                    if (status != "pass") out.detail = c.value("message", "");
                }
                if (name == "com_engine" && status != "pass") out.detail = c.value("message", "");
            }
        } catch (const std::exception& e) {
            out.scripting_available = SapState::Tri::No;
            out.detail = e.what();
        }
        return out;
    }

    DesktopState desktop() override {
        DesktopState d;
        DWORD session = 0;
        if (ProcessIdToSessionId(GetCurrentProcessId(), &session)) d.session_id = session;
        wchar_t name[256] = {};
        DWORD len = 0;
        if (GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, name, sizeof(name), &len))
            d.interactive_window_station = _wcsicmp(name, L"WinSta0") == 0;
        return d;
    }

    LockState lock_state() override {
        // Disconnected RDP session?
        LPWSTR buffer = nullptr;
        DWORD bytes = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSConnectState, &buffer, &bytes) &&
            buffer) {
            const auto state = *reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(buffer);
            WTSFreeMemory(buffer);
            if (state == WTSDisconnected) return LockState::RdpDisconnected;
        }
        // The input desktop is not accessible while the workstation is locked (Winlogon desktop).
        HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
        if (!input) return GetLastError() == ERROR_ACCESS_DENIED ? LockState::Locked : LockState::Unknown;
        CloseDesktop(input);
        return LockState::Active;
    }

    // Read-only: lists the Credential Manager token records; nullopt when they cannot be read.
    std::optional<int> token_count() override {
        try {
            auth::TokenStore store(auth::make_credential_manager_backend(auth::kTokenTargetPrefix));
            return count_active_tokens(store);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<std::string> autostart_command() override { return tray::make_win32_run_key()->get(tray::kAutostartValueName); }
    bool tray_running() override { return tray::make_win32_single_instance()->exists(); }

private:
    const HandlerProvider& get_handler_;
};

} // namespace

int run_mcp_doctor_command(McpExtras& x, const HandlerProvider& get_handler) {
    const auto env = process_env();
    const LoadedConfig loaded = load_config(x.config_path, env);

    DoctorInput input;
    input.config.path = loaded.path.path.string();
    input.config.exists = loaded.exists;
    input.config.read_error = loaded.read_error;
    input.config.issues = loaded.parsed.issues;
    input.exe_path = tray::current_exe_path();

    const Effective effective = resolve(loaded.parsed.config, flags_layer(*x.mcp_app), env);
    input.transport = std::get<std::string>(effective.at("server.transport").value);
    input.host = std::get<std::string>(effective.at("server.host").value);
    input.port = static_cast<int>(std::get<long long>(effective.at("server.port").value));

    RealProbes probes(get_handler);
    const auto checks = run_mcp_doctor(input, probes);
    if (x.output == "json") {
        nlohmann::json out;
        out["status"] = "success";
        out["data"] = doctor_to_json(checks);
        std::cout << out.dump(2) << std::endl;
    } else {
        std::cout << format_doctor_text(checks);
    }
    return overall_status(checks) == "fail" ? 1 : 0;
}

} // namespace commands
} // namespace fairyfly
