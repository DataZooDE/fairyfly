// Real SystemProbe: TCP probe (ws2_32), thumbprint-pinned TLS probe (WinHTTP), files, clock, names.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "include/setup/setup_hosts.h"
#include "winhttp_client.h"
#include "win_util.h"

namespace fairyfly::setup {

namespace {

using namespace win;
namespace fs = std::filesystem;

class RealSystemProbe : public SystemProbe {
public:
    bool tcp_listening(const std::string& host, int port) override {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
        bool listening = false;
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        const std::string h = (host.empty() || host == "+" || host == "localhost") ? "127.0.0.1" : host;
        if (getaddrinfo(h.c_str(), std::to_string(port).c_str(), &hints, &result) == 0) {
            for (addrinfo* a = result; a && !listening; a = a->ai_next) {
                SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (s == INVALID_SOCKET) continue;
                u_long nonblocking = 1;
                ioctlsocket(s, FIONBIO, &nonblocking);
                const int rc = connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
                if (rc == 0) {
                    listening = true;
                } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    fd_set writable, failed;
                    FD_ZERO(&writable);
                    FD_ZERO(&failed);
                    FD_SET(s, &writable);
                    FD_SET(s, &failed);
                    timeval timeout{1, 500000};
                    if (select(0, nullptr, &writable, &failed, &timeout) > 0 && FD_ISSET(s, &writable)) listening = true;
                }
                closesocket(s);
            }
            freeaddrinfo(result);
        }
        WSACleanup();
        return listening;
    }

    TlsProbe tls_probe(const std::string& host, int port, const std::string& expected) override {
        TlsProbe out;
        HttpProbeRequest req;
        req.host = host;
        req.port = port;
        req.tls = true;
        req.method = "GET";
        req.path = "/mcp";
        HttpProbeResponse r = winhttp_request(req);
        if (!r.ok && (r.win_error == ERROR_WINHTTP_NAME_NOT_RESOLVED || r.win_error == ERROR_WINHTTP_CANNOT_CONNECT || r.win_error == ERROR_WINHTTP_TIMEOUT) && host != "127.0.0.1") {
            req.connect_host = "127.0.0.1";   // the name may not resolve here; pin by thumbprint anyway
            r = winhttp_request(req);
        }
        if (!r.ok) {
            out.status = (r.win_error == ERROR_WINHTTP_SECURE_FAILURE || r.win_error == ERROR_WINHTTP_SECURE_CHANNEL_ERROR) ? "handshake_failed"
                         : (r.win_error == ERROR_WINHTTP_CANNOT_CONNECT || r.win_error == ERROR_WINHTTP_TIMEOUT || r.win_error == ERROR_WINHTTP_NAME_NOT_RESOLVED) ? "unreachable"
                                                                                                                                                                   : "error";
            out.detail = r.error;
            return out;
        }
        out.status = "ok";
        out.protocol = r.protocol;
        out.thumbprint = r.cert_thumbprint;
        out.thumbprint_match = !expected.empty() && r.cert_thumbprint == expected;
        out.http_status = r.status;
        return out;
    }

    std::optional<std::string> read_file(const std::string& path) override {
        std::ifstream in(win::to_path(path), std::ios::binary);
        if (!in) return std::nullopt;
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
        return text;
    }

    bool write_file(const std::string& path, const std::string& text) override {
        std::error_code ec;
        const fs::path target = win::to_path(path);
        if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        return static_cast<bool>(out);
    }

    bool remove_file(const std::string& path) override {
        std::error_code ec;
        return fs::remove(win::to_path(path), ec);
    }

    bool file_exists(const std::string& path) override {
        std::error_code ec;
        return fs::exists(win::to_path(path), ec);
    }

    long long now() override {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string computer_dns_name() override {
        for (const auto format : {ComputerNameDnsFullyQualified, ComputerNameDnsHostname, ComputerNameNetBIOS}) {
            DWORD size = 0;
            GetComputerNameExW(format, nullptr, &size);
            if (size == 0) continue;
            std::wstring name(size, L'\0');
            if (GetComputerNameExW(format, name.data(), &size)) {
                name.resize(size);
                if (!name.empty()) return narrow(name);
            }
        }
        return "localhost";
    }

    std::string local_app_data() override {
        char* value = nullptr;
        size_t size = 0;
        std::string out;
        if (_dupenv_s(&value, &size, "LOCALAPPDATA") == 0 && value) {
            out = value;
            free(value);
        }
        if (out.empty()) out = fs::temp_directory_path().string();
        return out;
    }
};

} // namespace

std::unique_ptr<SystemProbe> make_real_system_probe() { return std::make_unique<RealSystemProbe>(); }

} // namespace fairyfly::setup
