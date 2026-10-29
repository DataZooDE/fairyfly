// Real SystemProbe: TCP probe (ws2_32), thumbprint-pinned TLS probe (WinHTTP), files, clock, names.
#include <winsock2.h>
#include "include/config/environment.h"
#include "include/text/utf8.h"
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "include/setup/setup_hosts.h"
#include "include/setup/http_probe.h"
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
        return probe_tls(host, port, expected, winhttp_request);
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
                if (!name.empty()) return text::wide_to_utf8_lossy(name);
            }
        }
        return "localhost";
    }

    std::string data_dir() override { return text::path_utf8(config::Environment::process().data_dir()); }
};

} // namespace

std::unique_ptr<SystemProbe> make_real_system_probe() { return std::make_unique<RealSystemProbe>(); }

} // namespace fairyfly::setup
