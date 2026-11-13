#pragma once

// The Windows side of include/link/proxy_config.h: the fairyfly policy, the user's fairyfly setting, the browser
// policies (registry, Firefox's policies.json and who may change it) and the inputs of the process's resolver.
// Windows only (proxy_sources_win32.cpp); the registry keys are parameters so that tests read their own keys.

#ifdef _WIN32

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "include/link/proxy_config.h"

namespace fairyfly::link::proxycfg::win {

enum class Hive { machine, user };  // HKLM, HKCU

/// HKLM\SOFTWARE\Policies\DataZoo\fairyfly: ProxyMode, ProxyServer, ProxyPacUrl, ProxyBypass (REG_SZ).
PolicySetting read_fairyfly_policy(Hive hive = Hive::machine, const std::wstring& key = sysproxy::kPolicyKey);

inline constexpr const wchar_t* kEdgePolicyKey = L"SOFTWARE\\Policies\\Microsoft\\Edge";
inline constexpr const wchar_t* kChromePolicyKey = L"SOFTWARE\\Policies\\Google\\Chrome";
inline constexpr const wchar_t* kFirefoxPolicyKey = L"SOFTWARE\\Policies\\Mozilla\\Firefox";  // values in its "Proxy" subkey

/// ProxyMode, ProxyServer, ProxyPacUrl, ProxyBypassList, ProxySettings (JSON) and ProxyServerMode of one key.
std::optional<BrowserPolicy> read_chromium_policy(const std::string& browser, Hive hive, const std::wstring& key,
                                                  std::string* warning = nullptr);
/// <key>\Proxy: Mode, HTTPProxy, SSLProxy, AutoConfigURL, Passthrough (REG_SZ), UseHTTPProxyForAllProtocols (DWORD).
std::optional<BrowserPolicy> read_firefox_registry_policy(Hive hive, const std::wstring& key, std::string* warning = nullptr);
/// <install dir>\distribution\policies.json. admin = only administrators can change the file, its folder and the
/// install folder; otherwise it counts as the user's own setting.
std::optional<BrowserPolicy> read_firefox_policies_json(const std::filesystem::path& install_dir, std::string* warning = nullptr);
/// Where Firefox is installed: the registry's install directories (machine, then the user's own install), then the
/// Program Files defaults. Existing folders only, no duplicates.
std::vector<std::filesystem::path> firefox_install_dirs();

/// The owner and DACL of a file or folder (GetNamedSecurityInfo); readable = false when it cannot be read.
SecurityInfo security_info(const std::filesystem::path& path);

/// Where to look; the defaults are the real policy keys and Firefox folders.
struct BrowserRoots {
    std::wstring edge = kEdgePolicyKey;
    std::wstring chrome = kChromePolicyKey;
    std::wstring firefox = kFirefoxPolicyKey;
    Hive machine = Hive::machine;  ///< tests: Hive::user with keys of their own
    std::optional<std::vector<std::filesystem::path>> firefox_dirs;  ///< nullopt: firefox_install_dirs()
    bool user_hive = true;  ///< also HKCU for Edge and Chrome
};
/// Edge (HKLM, else HKCU), Chrome (HKLM, else HKCU), Firefox (HKLM registry, else the first policies.json); at most one
/// per browser, in that order.
std::vector<BrowserPolicy> read_browser_policies(std::vector<std::string>& warnings, const BrowserRoots& roots = {});

/// The user's setting (tray settings) and its warning.
std::optional<Setting> read_user_setting(std::string* warning = nullptr);
bool write_user_setting(const Setting& s);
/// The raw fairyfly policy and user values: cheap, polled by the running link to reconnect on a change.
std::string setting_fingerprint();

}  // namespace fairyfly::link::proxycfg::win

#endif
