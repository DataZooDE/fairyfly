#pragma once

// The link tray's "Network / proxy..." window: mode (Automatic / Manual / PAC script / Direct), the fields of the mode,
// the route in plain words, "Test connection", Save and Cancel; everything locked when the administrator's policy sets
// the proxy (include/link/proxy_config.h). The services are portable; the window is Win32 (proxy_window_win32.cpp).

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "include/link/proxy_config.h"

namespace fairyfly::tray {

struct ProxyWindowState {
    link::proxycfg::Setting setting;  ///< shown when it opens: the effective one (policy's, else the user's, else Auto)
    bool locked = false;
    std::string locked_text;          ///< "Set by your administrator (...)"
    std::string warnings;             ///< from reading the sources, one paragraph
};

struct ProxyWindowServices {
    std::function<ProxyWindowState()> load;
    /// The route for a setting not saved yet ("via ... from ..., Windows login: yes"); may evaluate a PAC script
    /// (seconds): called on a worker thread.
    std::function<std::string(const link::proxycfg::Setting&)> route;
    /// Saves the user setting; the reason when it cannot be saved (invalid, locked, store error).
    std::function<std::optional<std::string>(const link::proxycfg::Setting&)> save;
    /// "Test connection" with the setting in the window (locked: the effective one): called on a worker thread.
    std::function<link::proxycfg::ProbeResult(const link::proxycfg::Setting&)> test;
    /// After a successful save (e.g. a balloon "saved; the link reconnects").
    std::function<void()> saved;
};

/// The services of this process (proxy_cli.h process_services); `notify` shows the tray's balloon.
ProxyWindowServices make_proxy_window_services(std::function<void(const std::string&)> notify);

/// The text the window's form gives: what Save would store, or the reason it cannot (the checks of proxy_config.h,
/// with the window's words).
struct ProxyForm {
    link::proxycfg::Mode mode = link::proxycfg::Mode::automatic;
    std::string server, bypass, pac_url;
};
std::optional<std::string> form_error(const ProxyForm& f);
link::proxycfg::Setting form_setting(const ProxyForm& f);

#ifdef _WIN32
/// Opens the window modally on the calling (tray UI) thread and returns when it is closed. A second call while it is
/// open brings it to the front. A WM_QUIT (the tray's Quit) closes it and is passed on.
void show_proxy_window(const ProxyWindowServices& services);

/// For tests: the window without its own message loop (the caller pumps), shown without activation. nullptr on failure.
/// The window's controls have these ids.
void* create_proxy_window(const ProxyWindowServices& services);
enum ProxyWindowControl : int {
    kProxyAuto = 1001, kProxyManual, kProxyPac, kProxyDirect,
    kProxyServerEdit, kProxyBypassEdit, kProxyPacEdit,
    kProxyRouteText, kProxyTestText, kProxyLockText,
    kProxyTestButton, kProxySaveButton, kProxyCancelButton, kProxyNoteText,
};
#endif

}  // namespace fairyfly::tray
