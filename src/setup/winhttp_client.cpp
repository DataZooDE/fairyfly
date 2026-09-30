#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define SECURITY_WIN32
#include <sspi.h>
#include <schannel.h>
#include <wincrypt.h>
#include <winhttp.h>

#include "winhttp_client.h"
#include "win_util.h"

namespace fairyfly::setup {

namespace {

using namespace win;

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle = nullptr) : h(handle) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::string protocol_name(DWORD p) {
    if (p & 0x00002000) return "TLS 1.3";   // SP_PROT_TLS1_3_CLIENT
    if (p & 0x00000800) return "TLS 1.2";   // SP_PROT_TLS1_2_CLIENT
    if (p & 0x00000200) return "TLS 1.1";
    if (p & 0x00000080) return "TLS 1.0";
    if (p & 0x00000020) return "SSL 3.0";
    if (p & 0x00000008) return "SSL 2.0";
    return "";
}

} // namespace

HttpProbeResponse winhttp_request(const HttpProbeRequest& req) {
    HttpProbeResponse out;
    Handle session(WinHttpOpen(L"fairyfly-setup/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        out.win_error = GetLastError();
        out.error = error_text(out.win_error);
        return out;
    }
    WinHttpSetTimeouts(session.h, req.timeout_ms, req.timeout_ms, req.timeout_ms, req.timeout_ms);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 |
                      WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    // Offer every protocol so a weak machine policy shows up as a weak negotiated protocol (best effort).
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols &= ~WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    const std::string connect_host = req.connect_host.empty() ? req.host : req.connect_host;
    Handle connection(WinHttpConnect(session.h, widen(connect_host).c_str(), static_cast<INTERNET_PORT>(req.port), 0));
    if (!connection.h) {
        out.win_error = GetLastError();
        out.error = error_text(out.win_error);
        return out;
    }
    Handle request(WinHttpOpenRequest(connection.h, widen(req.method).c_str(), widen(req.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, req.tls ? WINHTTP_FLAG_SECURE : 0));
    if (!request.h) {
        out.win_error = GetLastError();
        out.error = error_text(out.win_error);
        return out;
    }
    if (req.tls) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request.h, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }
    DWORD no_redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &no_redirect, sizeof(no_redirect));
    std::wstring headers;
    if (connect_host != req.host) headers += L"Host: " + widen(req.host + ":" + std::to_string(req.port)) + L"\r\n";
    for (const auto& [k, v] : req.headers) headers += widen(k + ": " + v) + L"\r\n";
    const DWORD body_size = static_cast<DWORD>(req.body.size());
    const BOOL sent = WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                         headers.empty() ? 0 : static_cast<DWORD>(-1L), body_size ? const_cast<char*>(req.body.data()) : WINHTTP_NO_REQUEST_DATA,
                                         body_size, body_size, 0);
    if (!sent || !WinHttpReceiveResponse(request.h, nullptr)) {
        out.win_error = GetLastError();
        out.error = error_text(out.win_error);
        return out;
    }
    if (req.tls) {
        PCCERT_CONTEXT cert = nullptr;
        DWORD size = sizeof(cert);
        if (WinHttpQueryOption(request.h, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &size) && cert) {
            BYTE hash[20];
            DWORD hash_size = sizeof(hash);
            if (CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, hash, &hash_size)) out.cert_thumbprint = hex_upper(hash, hash_size);
            CertFreeCertificateContext(cert);
        }
        WINHTTP_SECURITY_INFO info{};
        size = sizeof(info);
        if (WinHttpQueryOption(request.h, WINHTTP_OPTION_SECURITY_INFO, &info, &size)) out.protocol = protocol_name(info.ConnectionInfo.dwProtocol);
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(status);
    DWORD wa_size = 0;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_WWW_AUTHENTICATE, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &wa_size, WINHTTP_NO_HEADER_INDEX);
    if (wa_size > 0) {
        std::wstring value(wa_size / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_WWW_AUTHENTICATE, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &wa_size, WINHTTP_NO_HEADER_INDEX)) {
            value.resize(wa_size / sizeof(wchar_t));
            out.www_authenticate = narrow(value);
        }
    }
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available) || available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), available, &read) || read == 0) break;
        out.body.append(chunk.data(), read);
        if (out.body.size() > 65536) break;
    }
    out.ok = true;
    return out;
}

} // namespace fairyfly::setup
