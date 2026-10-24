#include "include/update/updater.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#endif

// libffl: HTTPS with the platform trust store and the configured proxy
#include "crypto.hpp"
#include "http_client.hpp"

namespace fairyfly::update {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
/// A failure of GitHub's asset host that a new signed URL may not have (seen: 400 now and then, 2026-10-17/18).
struct AssetHostError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
bool worth_retrying(int status) { return status == 400 || status == 403 || status == 408 || status == 429 || status >= 500; }

std::string fetch_once(const std::string& start, size_t max_body, int max_redirects, const HttpGet& get) {
    std::string url = start;
    for (int hop = 0; hop <= max_redirects; hop++) {
        if (hop > 0 && !download_host_allowed(url)) throw std::runtime_error("update download redirected to a host that is not GitHub's: " + url);
        ffy::HttpRequest rq;
        rq.url = url;
        const bool api = url.rfind("https://api.github.com/", 0) == 0;
        // No User-Agent of our own: libffl's http_do always sends one, a second made the request carry two. An explicit
        // Accept-Encoding, or cpp-httplib sends the header empty. GitHub's asset origin (http.sys) rejects some
        // requests with 400 "Invalid Header" (2026-10-23); the request now looks like a plain client's.
        rq.headers = {{"Accept", api ? "application/vnd.github+json" : "application/octet-stream"}, {"Accept-Encoding", "identity"}};
        // optional, for rate limits on shared machines (CI); sent to the API only, never to a download host
        if (api)
            if (const char* t = std::getenv("FAIRYFLY_GITHUB_TOKEN"); t && *t) rq.headers.push_back({"Authorization", std::string("Bearer ") + t});
        rq.timeout_ms = 30000;
        rq.max_body = max_body;
        ffy::HttpResponse r;
        try {
            r = get(rq);
        } catch (const std::exception& e) {
            // a dropped connection is never GitHub's answer: start the download again (seen on github.com itself,
            // "Failed to read connection", 2026-10-24); the API keeps failing at once
            if (hop > 0 || !api) throw AssetHostError(e.what());
            throw;
        }
        if (r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308) {
            url = r.header("location");
            if (url.empty()) throw std::runtime_error("redirect without a location from " + rq.url);
            continue;
        }
        if (r.status != 200) {
            std::string why = "HTTP " + std::to_string(r.status);
            // which part answered: Azure blob storage behind GitHub's CDN (Fastly)
            std::string detail;
            for (const char* h : {"x-ms-error-code", "x-cache", "x-served-by"})
                if (const auto v = r.header(h); !v.empty()) detail += (detail.empty() ? "" : ", ") + std::string(h) + ": " + v;
            if (!detail.empty()) why += " (" + detail + ")";
            why += " from " + rq.url;
            if (hop > 0 && worth_retrying(r.status)) throw AssetHostError(why);
            throw std::runtime_error(why);
        }
        return r.body;
    }
    throw std::runtime_error("too many redirects from " + start);
}
}  // namespace

std::string fetch(const std::string& start, size_t max_body, int max_redirects, const HttpGet& get, const std::function<void(int ms)>& wait) {
    for (int attempt = 1;; attempt++) {
        try {
            return fetch_once(start, max_body, max_redirects, get);
        } catch (const AssetHostError&) {
            if (attempt >= kFetchAttempts) throw;
            wait(kFetchWaitsMs[attempt - 1]);
        }
    }
}

std::string fetch(const std::string& start, size_t max_body, int max_redirects) {
    return fetch(start, max_body, max_redirects, [](const ffy::HttpRequest& rq) { return ffy::http_do(rq); },
                 [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
}

CheckResult check(const std::string& current_version, const std::string& api_url) {
    auto body = fetch(api_url, 2u << 20);
    auto j = json::parse(body, nullptr, false);
    std::string why;
    auto rel = parse_release(j, why);
    if (!rel) throw std::runtime_error("the latest release cannot be used: " + why);
    CheckResult c;
    c.current = current_version;
    c.latest = rel->version;
    c.page = rel->page_url;
    c.release = *rel;
    auto a = parse_version(current_version), b = parse_version(rel->version);
    c.available = a && b && compare(*b, *a) > 0;
    return c;
}

std::string sha256_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ffy::hex_encode(ffy::sha256(ss.str()));
}

#ifdef _WIN32
Signature authenticode(const fs::path& file) {
    Signature s;
    const std::wstring path = file.wstring();
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = path.c_str();
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG rc = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    s.valid = rc == ERROR_SUCCESS;

    // the signer's subject (also for an invalid signature, so the message can name it)
    HCERTSTORE store = nullptr;
    HCRYPTMSG msg = nullptr;
    if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                         nullptr, nullptr, nullptr, &store, &msg, nullptr)) {
        DWORD size = 0;
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &size) && size) {
            std::string buf(size, '\0');
            auto* info = reinterpret_cast<CMSG_SIGNER_INFO*>(buf.data());
            if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, info, &size)) {
                CERT_INFO ci{};
                ci.Issuer = info->Issuer;
                ci.SerialNumber = info->SerialNumber;
                if (PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci, nullptr)) {
                    DWORD n = CertNameToStrW(X509_ASN_ENCODING, &cert->pCertInfo->Subject, CERT_X500_NAME_STR, nullptr, 0);
                    std::wstring w(n, L'\0');
                    CertNameToStrW(X509_ASN_ENCODING, &cert->pCertInfo->Subject, CERT_X500_NAME_STR, w.data(), n);
                    if (!w.empty() && w.back() == L'\0') w.pop_back();
                    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
                    s.subject.assign(static_cast<size_t>(len), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.subject.data(), len, nullptr, nullptr);
                    CertFreeCertificateContext(cert);
                }
            }
        }
    }
    if (msg) CryptMsgClose(msg);
    if (store) CertCloseStore(store, 0);
    if (!s.valid && s.subject.empty()) s.subject.clear();
    return s;
}

fs::path current_exe() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return fs::path(buf);
}
#else
Signature authenticode(const fs::path&) { return {}; }
fs::path current_exe() {
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
}
#endif

ApplyResult apply(const std::string& current_version, bool allow_unsigned, const std::string& api_url) {
    ApplyResult out;
    auto c = check(current_version, api_url);
    Facts f;
    f.current_version = current_version;
    f.release_version = c.latest;
    f.allow_unsigned = allow_unsigned;
    if (!c.available) {
        out.decision = decide(f);
        return out;
    }
    const fs::path exe = current_exe();
    const fs::path next = fs::path(exe.string() + ".new"), old = fs::path(exe.string() + ".old");
    std::error_code ec;
    fs::remove(old, ec);  // left over from the previous update
    const std::string sums = fetch(c.release.sums_url, 64u << 10);
    if (auto h = expected_sha256(sums, "fairyfly.exe")) f.expected_sha256 = *h;
    {
        const std::string bin = fetch(c.release.exe_url, 256u << 20);
        std::ofstream o(next, std::ios::binary | std::ios::trunc);
        o.write(bin.data(), static_cast<std::streamsize>(bin.size()));
        if (!o) throw std::runtime_error("cannot write " + next.string());
    }
    f.actual_sha256 = sha256_file(next);
    f.current = authenticode(exe);
    f.downloaded = authenticode(next);
    out.decision = decide(f);
    if (out.decision.verdict != Verdict::Install) {
        fs::remove(next, ec);
        return out;
    }
    // a running executable can be renamed on Windows (not overwritten): move it aside, put the new one in place
    fs::rename(exe, old, ec);
    if (ec) {
        fs::remove(next, ec);
        throw std::runtime_error("cannot move the running fairyfly.exe aside: " + ec.message());
    }
    fs::rename(next, exe, ec);
    if (ec) {
        std::error_code ec2;
        fs::rename(old, exe, ec2);  // roll back
        throw std::runtime_error("cannot put the new fairyfly.exe in place (" + ec.message() + "); the old one was restored");
    }
    out.installed_path = exe.string();
    out.previous_path = old.string();
    return out;
}

}  // namespace fairyfly::update
