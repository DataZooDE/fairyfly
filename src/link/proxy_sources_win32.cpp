// The Windows sources of the proxy decision (include/link/proxy_sources.h): the fairyfly policy and the user's setting
// in the registry, the Edge/Chrome/Firefox proxy policies, Firefox's policies.json and who can change it, and the
// environment overrides hidden from libffl. Values are read, never logged with credentials.
#include "include/link/proxy_sources.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "include/text/utf8.h"
#include "include/tray/tray_contract.h"
#include "util.hpp"  // libffl

namespace fairyfly::link::proxycfg {

// HTTPS_PROXY / NO_PROXY: libffl reads them with getenv, i.e. from the C runtime's copy of the environment. They are
// removed from that copy (and _putenv_s removes them from the process environment block too), then put back into the
// block alone: libffl no longer sees them, child processes (the tray's link, the MCP server) inherit them unchanged.
EnvOverrides take_env_overrides() {
    EnvOverrides e;
    e.https_proxy = ffy::env_or("HTTPS_PROXY", ffy::env_or("https_proxy"));
    e.no_proxy = ffy::env_or("NO_PROXY", ffy::env_or("no_proxy"));
    for (const wchar_t* name : {L"HTTPS_PROXY", L"NO_PROXY"}) {  // names are case-insensitive on Windows
        std::wstring saved;
        const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
        if (n > 0) {
            saved.resize(n);
            const DWORD got = GetEnvironmentVariableW(name, saved.data(), n);
            saved.resize(got < n ? got : 0);
        }
        _wputenv_s(name, L"");
        if (!saved.empty()) SetEnvironmentVariableW(name, saved.c_str());
    }
    return e;
}

namespace win {

namespace {

HKEY root_of(Hive h) { return h == Hive::machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }
std::string hive_name(Hive h) { return h == Hive::machine ? "HKLM" : "HKCU"; }

std::optional<std::string> reg_string(HKEY root, const std::wstring& key, const wchar_t* value) {
    DWORD size = 0;
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return std::nullopt;
    if (size > 64 * 1024) return std::string();  // absurd: treated as empty (and then refused by the checks)
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    DWORD got = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf.data(), &got) != ERROR_SUCCESS)
        return std::nullopt;
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    return text::wide_to_utf8_lossy(buf);
}

std::optional<std::uint32_t> reg_dword(HKEY root, const std::wstring& key, const wchar_t* value) {
    DWORD v = 0, size = sizeof v;
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return std::nullopt;
    return v;
}

std::string key_label(Hive h, const std::wstring& key) { return hive_name(h) + "\\" + text::wide_to_utf8_lossy(key); }

const std::wstring kTrustedInstaller = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

bool admin_like(PSID sid) {
    if (!sid || !IsValidSid(sid)) return false;
    if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || IsWellKnownSid(sid, WinCreatorOwnerSid))
        return true;
    LPWSTR s = nullptr;
    if (!ConvertSidToStringSidW(sid, &s)) return false;
    const bool ti = kTrustedInstaller == s;
    LocalFree(s);
    return ti;
}

}  // namespace

PolicySetting read_fairyfly_policy(Hive hive, const std::wstring& key) {
    const HKEY root = root_of(hive);
    return policy_from_values(reg_string(root, key, kPolicyModeValue), reg_string(root, key, kPolicyServerValue),
                              reg_string(root, key, kPolicyPacValue), reg_string(root, key, kPolicyBypassValue));
}

std::optional<BrowserPolicy> read_chromium_policy(const std::string& browser, Hive hive, const std::wstring& key, std::string* warning) {
    const HKEY root = root_of(hive);
    ChromiumValues v;
    v.mode = reg_string(root, key, L"ProxyMode");
    v.server = reg_string(root, key, L"ProxyServer");
    v.pac_url = reg_string(root, key, L"ProxyPacUrl");
    v.bypass = reg_string(root, key, L"ProxyBypassList");
    v.settings_json = reg_string(root, key, L"ProxySettings");
    v.server_mode = reg_dword(root, key, L"ProxyServerMode");
    return chromium_policy(browser, key_label(hive, key), v, warning);
}

std::optional<BrowserPolicy> read_firefox_registry_policy(Hive hive, const std::wstring& key, std::string* warning) {
    const HKEY root = root_of(hive);
    const std::wstring proxy = key + L"\\Proxy";
    FirefoxValues v;
    v.mode = reg_string(root, proxy, L"Mode");
    v.http_proxy = reg_string(root, proxy, L"HTTPProxy");
    v.ssl_proxy = reg_string(root, proxy, L"SSLProxy");
    v.autoconfig_url = reg_string(root, proxy, L"AutoConfigURL");
    v.passthrough = reg_string(root, proxy, L"Passthrough");
    if (auto d = reg_dword(root, proxy, L"UseHTTPProxyForAllProtocols")) v.http_for_all = *d != 0;
    return firefox_policy(key_label(hive, proxy), true, v, warning);
}

SecurityInfo security_info(const std::filesystem::path& path) {
    SecurityInfo info;
    PSECURITY_DESCRIPTOR sd = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    const DWORD rc = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                                           nullptr, &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) return info;
    info.readable = true;
    info.owner_admin_like = admin_like(owner);
    info.has_dacl = dacl != nullptr;
    if (dacl) {
        for (DWORD i = 0; i < dacl->AceCount; ++i) {
            void* raw = nullptr;
            if (!GetAce(dacl, i, &raw)) continue;
            const auto* h = static_cast<const ACE_HEADER*>(raw);
            Ace a;
            a.inherit_only = (h->AceFlags & INHERIT_ONLY_ACE) != 0;
            if (h->AceType == ACCESS_ALLOWED_ACE_TYPE) {
                const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
                a.allow = true;
                a.mask = ace->Mask;
                a.admin_like = admin_like(const_cast<DWORD*>(&ace->SidStart));
            } else if (h->AceType == ACCESS_DENIED_ACE_TYPE) {
                const auto* ace = static_cast<const ACCESS_DENIED_ACE*>(raw);
                a.allow = false;
                a.mask = ace->Mask;
            } else {
                // object, callback or conditional entries: not understood here, so counted as "anyone may change it"
                a.allow = true;
                a.mask = GENERIC_ALL;
            }
            info.aces.push_back(a);
        }
    }
    LocalFree(sd);
    return info;
}

std::optional<BrowserPolicy> read_firefox_policies_json(const std::filesystem::path& install_dir, std::string* warning) {
    std::error_code ec;
    const auto dist = install_dir / "distribution";
    const auto file = dist / "policies.json";
    if (!std::filesystem::is_regular_file(file, ec)) return std::nullopt;
    if (std::filesystem::file_size(file, ec) > 1024 * 1024) {
        if (warning) *warning = "Firefox policies.json is larger than 1 MB; ignored";
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    std::string error;
    FirefoxValues v = firefox_values_from_json(buf.str(), &error);
    if (!error.empty()) {
        if (warning) *warning = "Firefox " + error + " (" + text::path_utf8(file) + "); ignored";
        return std::nullopt;
    }
    if (!v.any()) return std::nullopt;
    // policy trust only when no one but administrators can change the file, its folder or the install folder (else
    // anyone who can write there could name their own proxy as "policy"): otherwise it is the user's own setting
    const bool admin = only_admins_can_change(security_info(file), false) && only_admins_can_change(security_info(dist), true) &&
                       only_admins_can_change(security_info(install_dir), true);
    return firefox_policy(text::path_utf8(file), admin, v, warning);
}

std::vector<std::filesystem::path> firefox_install_dirs() {
    std::vector<std::filesystem::path> out;
    auto add = [&](const std::filesystem::path& p) {
        std::error_code ec;
        if (p.empty() || !std::filesystem::is_directory(p, ec)) return;
        for (const auto& o : out)
            if (std::filesystem::equivalent(o, p, ec)) return;
        out.push_back(p);
    };
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        for (const wchar_t* product : {L"SOFTWARE\\Mozilla\\Mozilla Firefox", L"SOFTWARE\\Mozilla\\Mozilla Firefox ESR"}) {
            auto ver = reg_string(root, product, L"CurrentVersion");
            if (!ver || ver->empty()) continue;
            std::wstring main = std::wstring(product) + L"\\" + text::utf8_to_wide_lossy(*ver) + L"\\Main";
            if (auto dir = reg_string(root, main, L"Install Directory")) add(std::filesystem::path(text::utf8_to_wide_lossy(*dir)));
        }
    }
    for (const wchar_t* env : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(env, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) add(std::filesystem::path(buf) / L"Mozilla Firefox");
    }
    return out;
}

std::vector<BrowserPolicy> read_browser_policies(std::vector<std::string>& warnings, const BrowserRoots& roots) {
    std::vector<BrowserPolicy> out;
    auto note = [&](std::string& w) {
        if (!w.empty()) warnings.push_back(w);
        w.clear();
    };
    std::string w;
    const std::pair<std::string, std::wstring> chromium[] = {{"Edge", roots.edge}, {"Chrome", roots.chrome}};
    for (const auto& [browser, key] : chromium) {
        auto p = read_chromium_policy(browser, roots.machine, key, &w);
        note(w);
        if (!p && roots.user_hive) {
            p = read_chromium_policy(browser, Hive::user, key, &w);
            note(w);
        }
        if (p) out.push_back(*p);
    }
    auto ff = read_firefox_registry_policy(roots.machine, roots.firefox, &w);
    note(w);
    if (!ff) {
        for (const auto& dir : roots.firefox_dirs ? *roots.firefox_dirs : firefox_install_dirs()) {
            ff = read_firefox_policies_json(dir, &w);
            note(w);
            if (ff) break;
        }
    }
    if (ff) out.push_back(*ff);
    return out;
}

std::optional<Setting> read_user_setting(std::string* warning) {
    const std::wstring key = tray::contract::kSettingsKey;
    return load_user_setting([&](const std::string& name) { return reg_string(HKEY_CURRENT_USER, key, text::utf8_to_wide_lossy(name).c_str()); },
                             warning);
}

bool write_user_setting(const Setting& s) {
    const std::wstring key = tray::contract::kSettingsKey;
    return save_user_setting(
        [&](const std::string& name, const std::string& value) {
            const std::wstring n = text::utf8_to_wide_lossy(name), v = text::utf8_to_wide_lossy(value);
            return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), n.c_str(), REG_SZ, v.c_str(),
                                   static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        },
        s);
}

std::string setting_fingerprint() {
    std::string fp;
    auto add = [&](const std::optional<std::string>& v) { fp += (v ? "1" + *v : std::string("0")) + '\x1f'; };
    const std::wstring policy = sysproxy::kPolicyKey;
    for (const wchar_t* v : {kPolicyModeValue, kPolicyServerValue, kPolicyPacValue, kPolicyBypassValue}) add(reg_string(HKEY_LOCAL_MACHINE, policy, v));
    const std::wstring user = tray::contract::kSettingsKey;
    for (const char* v : {kUserModeSetting, kUserServerSetting, kUserBypassSetting, kUserPacSetting})
        add(reg_string(HKEY_CURRENT_USER, user, text::utf8_to_wide_lossy(v).c_str()));
    add(reg_dword(HKEY_LOCAL_MACHINE, policy, sysproxy::kDiscoveredLoginPolicy) ? std::optional<std::string>("p") : std::nullopt);
    return fp;
}

}  // namespace win
}  // namespace fairyfly::link::proxycfg
