// The Windows sources of the proxy decision (include/link/proxy_sources.h) for real: the policy readers against
// registry keys of this test (HKCU\Software\fairyfly-tests\...), Firefox's policies.json and the ACL check on real
// files, the environment hidden from libffl but inherited by a child process, WinHTTP evaluating a real PAC script, and
// the "Network / proxy..." window (with screenshots into FAIRYFLY_TEST_SCREENSHOTS when set: CI uploads them).
#ifdef _WIN32
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <windows.h>

#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "include/link/proxy_sources.h"
#include "include/link/trail_recorder.h"
#include "include/tray/proxy_window.h"

using namespace fairyfly::link::proxycfg;
using namespace fairyfly::link::proxycfg::win;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

// A registry key of this test under HKCU, removed afterwards.
class TestKey {
public:
    explicit TestKey(const std::wstring& name)
        : path_(L"Software\\fairyfly-tests\\proxy-" + std::to_wstring(GetCurrentProcessId()) + L"\\" + name) {}
    ~TestKey() { RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str()); }
    void set(const std::wstring& value, const std::wstring& data, const std::wstring& sub = L"") const {
        const std::wstring k = sub.empty() ? path_ : path_ + L"\\" + sub;
        REQUIRE(RegSetKeyValueW(HKEY_CURRENT_USER, k.c_str(), value.c_str(), REG_SZ, data.c_str(),
                                static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
    }
    void set_dword(const std::wstring& value, DWORD v, const std::wstring& sub = L"") const {
        const std::wstring k = sub.empty() ? path_ : path_ + L"\\" + sub;
        REQUIRE(RegSetKeyValueW(HKEY_CURRENT_USER, k.c_str(), value.c_str(), REG_DWORD, &v, sizeof v) == ERROR_SUCCESS);
    }
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

fs::path temp_dir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("ffy-proxy-" + std::to_string(GetCurrentProcessId()) + "-" + name);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void pump(std::chrono::milliseconds total) {
    const auto end = std::chrono::steady_clock::now() + total;
    MSG m;
    while (std::chrono::steady_clock::now() < end) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::wstring text(HWND h, int id) {
    wchar_t buf[2048];
    GetWindowTextW(GetDlgItem(h, id), buf, 2048);
    return buf;
}

// PrintWindow into a PNG under FAIRYFLY_TEST_SCREENSHOTS (nothing when it is not set).
void screenshot(HWND h, const std::string& name) {
    const char* dir = std::getenv("FAIRYFLY_TEST_SCREENSHOTS");
    if (!dir || !*dir) return;
    RECT r{};
    GetWindowRect(h, &r);
    const int w = r.right - r.left, ht = r.bottom - r.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -ht;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(h, mem, PW_RENDERFULLCONTENT);
    fairyfly::link::trail::Image img;
    img.width = w;
    img.height = ht;
    img.rgb.resize(size_t(w) * size_t(ht) * 3);
    const auto* px = static_cast<const std::uint8_t*>(bits);
    for (size_t i = 0; i < size_t(w) * size_t(ht); ++i) {
        img.rgb[i * 3] = px[i * 4 + 2];
        img.rgb[i * 3 + 1] = px[i * 4 + 1];
        img.rgb[i * 3 + 2] = px[i * 4];
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    fs::create_directories(dir);
    std::ofstream(fs::path(dir) / (name + ".png"), std::ios::binary) << fairyfly::link::trail::encode_png(img);
}

}  // namespace

TEST_CASE("proxy sources (Windows): the fairyfly policy as the registry holds it", "[proxy][win32]") {
    TestKey k(L"policy");
    CHECK_FALSE(read_fairyfly_policy(Hive::user, k.path()).present);
    k.set(L"ProxyServer", L"proxy.corp.example:8080");
    CHECK_FALSE(read_fairyfly_policy(Hive::user, k.path()).present);  // no ProxyMode: no lock
    k.set(L"ProxyMode", L"manual");
    k.set(L"ProxyBypass", L"<local>;*.corp.example");
    auto p = read_fairyfly_policy(Hive::user, k.path());
    CHECK(p.present);
    CHECK(p.error.empty());
    CHECK(p.setting.server == "proxy.corp.example:8080");
    CHECK(p.setting.bypass == "<local>;*.corp.example");
    k.set(L"ProxyServer", L"socks5://proxy:1080");
    p = read_fairyfly_policy(Hive::user, k.path());
    CHECK(p.present);
    CHECK_THAT(p.error, ContainsSubstring("SOCKS"));
    // the real policy key of this machine reads without error (CI has none)
    CHECK_NOTHROW(read_fairyfly_policy());
}

TEST_CASE("proxy sources (Windows): Edge/Chrome and Firefox policies from the registry, in order", "[proxy][win32]") {
    TestKey edge(L"edge"), chrome(L"chrome"), firefox(L"firefox");
    chrome.set(L"ProxyMode", L"pac_script");
    chrome.set(L"ProxyPacUrl", L"https://pac.corp.example/p.pac");
    firefox.set(L"Mode", L"manual", L"Proxy");
    firefox.set(L"HTTPProxy", L"ff.corp.example:3128", L"Proxy");
    firefox.set_dword(L"UseHTTPProxyForAllProtocols", 1, L"Proxy");

    BrowserRoots roots;
    roots.machine = Hive::user;
    roots.user_hive = false;
    roots.edge = edge.path();
    roots.chrome = chrome.path();
    roots.firefox = firefox.path();
    roots.firefox_dirs = std::vector<fs::path>{};
    std::vector<std::string> warnings;
    auto list = read_browser_policies(warnings, roots);
    REQUIRE(list.size() == 2);
    CHECK(list[0].browser == "Chrome");
    CHECK(list[0].kind == BrowserPolicy::Kind::pac);
    CHECK(list[0].label() == "Chrome policy (HKCU)");
    CHECK(list[1].browser == "Firefox");
    CHECK(list[1].kind == BrowserPolicy::Kind::fixed);
    CHECK(list[1].server == "ff.corp.example:3128");
    CHECK(warnings.empty());

    edge.set(L"ProxySettings", LR"({"ProxyMode":"fixed_servers","ProxyServer":"edge.corp.example:8080"})");
    edge.set_dword(L"ProxyServerMode", 0);  // overridden by ProxySettings
    list = read_browser_policies(warnings, roots);
    REQUIRE(list.size() == 3);
    CHECK(list[0].browser == "Edge");
    CHECK(list[0].server == "edge.corp.example:8080");

    edge.set(L"ProxySettings", L"{broken");
    warnings.clear();
    list = read_browser_policies(warnings, roots);
    CHECK(list[0].kind == BrowserPolicy::Kind::direct);  // ProxyServerMode 0, with a warning about ProxySettings
    REQUIRE_FALSE(warnings.empty());
    CHECK_THAT(warnings[0], ContainsSubstring("ProxySettings"));
}

TEST_CASE("proxy sources (Windows): a policies.json in a folder users can write counts as the user's own", "[proxy][win32]") {
    const fs::path install = temp_dir("ff");
    fs::create_directories(install / "distribution");
    std::ofstream(install / "distribution" / "policies.json")
        << R"({"policies":{"Proxy":{"Mode":"autoConfig","AutoConfigURL":"http://pac.corp.example/p.pac"}}})";
    std::string w;
    auto p = read_firefox_policies_json(install, &w);
    REQUIRE(p);
    CHECK(p->kind == BrowserPolicy::Kind::pac);
    CHECK_FALSE(p->admin);  // %TEMP% is the user's: not policy
    CHECK(p->label() == "Firefox policies.json (user-writable)");
    // so its http:// script does not get the Windows login, unlike the same script from an administrator's policy
    Inputs in;
    in.browsers = {*p};
    auto d = decide(in, "https://fairyfly.io/", [](const std::string&, fairyfly::link::sysproxy::AutoSource, const std::string&, bool) {
        fairyfly::link::sysproxy::AutoProxyResult r;
        r.ok = true;
        r.proxy = "p:1";
        return r;
    });
    CHECK(d.routes.front().proxy.origin == ffy::proxy::Origin::discovered);

    std::ofstream(install / "distribution" / "policies.json") << "{";
    w.clear();
    CHECK_FALSE(read_firefox_policies_json(install, &w));
    CHECK_THAT(w, ContainsSubstring("not valid JSON"));
    CHECK_FALSE(read_firefox_policies_json(temp_dir("none"), &w));
    fs::remove_all(install);
    CHECK_NOTHROW(firefox_install_dirs());
}

TEST_CASE("proxy sources (Windows): who may change a file, from its real security descriptor", "[proxy][win32]") {
    wchar_t sys[MAX_PATH];
    REQUIRE(GetSystemDirectoryW(sys, MAX_PATH) > 0);
    const fs::path hosts = fs::path(sys) / "drivers" / "etc" / "hosts";
    CHECK(only_admins_can_change(security_info(hosts), false));
    CHECK(only_admins_can_change(security_info(fs::path(sys)), true));
    wchar_t pf[MAX_PATH];
    REQUIRE(GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH) > 0);
    CHECK(only_admins_can_change(security_info(fs::path(pf)), true));

    const fs::path mine = temp_dir("acl") / "policies.json";
    std::ofstream(mine) << "{}";
    const SecurityInfo info = security_info(mine);
    CHECK(info.readable);
    CHECK_FALSE(only_admins_can_change(info, false));
    CHECK_FALSE(only_admins_can_change(security_info(mine.parent_path()), true));
    CHECK_FALSE(security_info(mine.parent_path() / "missing").readable);
    fs::remove_all(mine.parent_path());
}

TEST_CASE("proxy sources (Windows): HTTPS_PROXY is hidden from libffl but child processes still get it", "[proxy][win32]") {
    _putenv_s("HTTPS_PROXY", "http://env.corp.example:3128");
    _putenv_s("NO_PROXY", ".corp.example");
    const EnvOverrides e = take_env_overrides();
    CHECK(e.https_proxy == "http://env.corp.example:3128");
    CHECK(e.no_proxy == ".corp.example");
    CHECK(std::getenv("HTTPS_PROXY") == nullptr);   // libffl reads this copy
    CHECK(std::getenv("https_proxy") == nullptr);
    wchar_t buf[256];
    CHECK(GetEnvironmentVariableW(L"HTTPS_PROXY", buf, 256) > 0);  // the process environment block keeps it

    // a child process inherits it: cmd exits 7 when it sees the variable
    std::wstring cmd = L"cmd.exe /d /c if defined HTTPS_PROXY (exit 7) else (exit 3)";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CHECK(code == 7);
    SetEnvironmentVariableW(L"HTTPS_PROXY", nullptr);
    SetEnvironmentVariableW(L"NO_PROXY", nullptr);
}

TEST_CASE("proxy sources (Windows): WinHTTP evaluates a real PAC script at a configured address", "[proxy][win32]") {
    fairyfly::link::sysproxy::install_system_proxy();
    auto resolver = process_resolver();
    REQUIRE(resolver);
    REQUIRE(resolver->sources().autoproxy);
    httplib::Server srv;
    srv.Get("/proxy.pac", [](const httplib::Request&, httplib::Response& r) {
        r.set_content(R"(function FindProxyForURL(url, host) {
            if (dnsDomainIs(host, ".fairyfly.io") || host == "fairyfly.io") return "PROXY pac-proxy.corp.example:3128; DIRECT";
            return "DIRECT"; })",
                      "application/x-ns-proxy-autoconfig");
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    std::thread th([&] { srv.listen_after_bind(); });
    srv.wait_until_ready();
    const std::string pac = "http://127.0.0.1:" + std::to_string(port) + "/proxy.pac";
    auto r = resolver->sources().autoproxy("https://fairyfly.io/health", fairyfly::link::sysproxy::AutoSource::pac, pac, false);
    INFO(r.error);
    CHECK(r.ok);
    CHECK_THAT(r.proxy, ContainsSubstring("pac-proxy.corp.example:3128"));
    // through the decision: your PAC setting names that proxy, an http:// script of yours gets no login
    Inputs in;
    Setting s;
    s.mode = Mode::pac;
    s.pac_url = pac;
    in.user = s;
    auto d = decide(in, "https://fairyfly.io/health", resolver->sources().autoproxy);
    REQUIRE_FALSE(d.routes.empty());
    CHECK(d.routes.front().proxy.host == "pac-proxy.corp.example");
    CHECK(d.routes.front().proxy.origin == ffy::proxy::Origin::discovered);
    CHECK(decide(in, "https://github.com/", resolver->sources().autoproxy).routes.empty());
    srv.stop();
    th.join();
}

TEST_CASE("proxy window (Windows): the form, Save, and the locked view - with screenshots", "[proxy][win32][window]") {
    using namespace fairyfly::tray;
    std::optional<Setting> saved;
    std::vector<Setting> routed;
    ProxyWindowServices s;
    s.load = [] {
        ProxyWindowState st;
        st.setting.mode = Mode::manual;
        st.setting.server = "proxy.corp.example:8080";
        st.setting.bypass = "<local>;*.corp.example";
        return st;
    };
    s.route = [&](const Setting& x) {
        routed.push_back(x);
        return std::string("via proxy.corp.example:8080 from your fairyfly setting, Windows login: yes");
    };
    s.save = [&](const Setting& x) -> std::optional<std::string> {
        saved = x;
        return std::nullopt;
    };
    s.test = [](const Setting&) {
        ProbeResult r;
        r.ok = true;
        r.summary = "Connected to link.fairyfly.io (HTTPS and the link's WebSocket) via proxy.corp.example:8080 from your fairyfly setting, Windows login: yes.";
        return r;
    };
    HWND h = static_cast<HWND>(create_proxy_window(s));
    REQUIRE(h);
    pump(std::chrono::milliseconds(300));
    CHECK(SendMessageW(GetDlgItem(h, kProxyManual), BM_GETCHECK, 0, 0) == BST_CHECKED);
    CHECK(text(h, kProxyServerEdit) == L"proxy.corp.example:8080");
    CHECK(IsWindowEnabled(GetDlgItem(h, kProxyServerEdit)));
    CHECK_FALSE(IsWindowEnabled(GetDlgItem(h, kProxyPacEdit)));
    CHECK(text(h, kProxyRouteText).find(L"via proxy.corp.example:8080") != std::wstring::npos);
    CHECK(text(h, kProxyNoteText).find(L"no SOCKS") != std::wstring::npos);
    SendMessageW(GetDlgItem(h, kProxyTestButton), BM_CLICK, 0, 0);
    pump(std::chrono::milliseconds(300));
    CHECK(text(h, kProxyTestText).find(L"Test: OK.") != std::wstring::npos);
    screenshot(h, "proxy-window-manual");

    // PAC: the other field opens; Save stores what the form says
    SendMessageW(GetDlgItem(h, kProxyPac), BM_CLICK, 0, 0);
    CHECK_FALSE(IsWindowEnabled(GetDlgItem(h, kProxyServerEdit)));
    CHECK(IsWindowEnabled(GetDlgItem(h, kProxyPacEdit)));
    SetWindowTextW(GetDlgItem(h, kProxyPacEdit), L"https://pac.corp.example/proxy.pac");
    SendMessageW(GetDlgItem(h, kProxySaveButton), BM_CLICK, 0, 0);
    pump(std::chrono::milliseconds(100));
    REQUIRE(saved);
    CHECK(saved->mode == Mode::pac);
    CHECK(saved->pac_url == "https://pac.corp.example/proxy.pac");
    CHECK(saved->server.empty());
    CHECK_FALSE(IsWindow(h));  // closed by Save

    // locked by the administrator's policy: nothing can be changed, the test still runs
    s.load = [] {
        ProxyWindowState st;
        st.locked = true;
        st.locked_text = fairyfly::link::proxycfg::kLockedText;
        st.setting.mode = Mode::pac;
        st.setting.pac_url = "https://pac.corp.example/proxy.pac";
        return st;
    };
    s.route = [](const Setting&) { return std::string("via pac-proxy.corp.example:3128 from PAC (https://pac.corp.example/proxy.pac, fairyfly policy), Windows login: yes"); };
    h = static_cast<HWND>(create_proxy_window(s));
    REQUIRE(h);
    pump(std::chrono::milliseconds(300));
    CHECK(text(h, kProxyLockText).find(L"Set by your administrator") != std::wstring::npos);
    for (int id : {kProxyAuto, kProxyManual, kProxyPac, kProxyDirect, kProxyPacEdit, kProxyServerEdit, kProxySaveButton})
        CHECK_FALSE(IsWindowEnabled(GetDlgItem(h, id)));
    CHECK(IsWindowEnabled(GetDlgItem(h, kProxyTestButton)));
    screenshot(h, "proxy-window-locked");
    SendMessageW(GetDlgItem(h, kProxyCancelButton), BM_CLICK, 0, 0);
    pump(std::chrono::milliseconds(100));
    CHECK_FALSE(IsWindow(h));
}
#endif
