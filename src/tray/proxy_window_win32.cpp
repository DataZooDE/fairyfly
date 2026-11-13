// The link tray's "Network / proxy..." window (include/tray/proxy_window.h): plain Win32 controls in the system's
// message font, as the tray's other windows (the approval task dialog, the message boxes) look. The route and the
// connection test run on worker threads and post their text back; the window never blocks the tray.
#include "include/tray/proxy_window.h"

#include <windows.h>
#include <commctrl.h>  // EM_SETCUEBANNER

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "include/text/utf8.h"
#include "include/tray/tray_resource.h"

namespace fairyfly::tray {

namespace {

using link::proxycfg::Mode;
using link::proxycfg::Setting;

constexpr UINT kRouteMessage = WM_APP + 41;  // lParam: new std::string, the route text
constexpr UINT kTestMessage = WM_APP + 42;   // lParam: new std::string, the test result
constexpr wchar_t kClass[] = L"fairyfly_proxy_window";

std::wstring w(const std::string& s) { return text::utf8_to_wide_lossy(s); }
std::string u(const std::wstring& s) { return text::wide_to_utf8_lossy(s); }

struct Window {
    ProxyWindowServices services;
    ProxyWindowState state;
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    bool modal_done = false;
    // shared with the worker threads: they post only while the window lives
    std::shared_ptr<std::atomic<HWND>> alive = std::make_shared<std::atomic<HWND>>(nullptr);
    int route_generation = 0;
};

Window* g_open = nullptr;  // the one open window of this process (UI thread only)

HWND item(HWND h, int id) { return GetDlgItem(h, id); }

std::string text_of(HWND h, int id) {
    HWND c = item(h, id);
    const int n = GetWindowTextLengthW(c);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(c, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return u(s);
}

Mode mode_of(HWND h) {
    if (SendMessageW(item(h, kProxyManual), BM_GETCHECK, 0, 0) == BST_CHECKED) return Mode::manual;
    if (SendMessageW(item(h, kProxyPac), BM_GETCHECK, 0, 0) == BST_CHECKED) return Mode::pac;
    if (SendMessageW(item(h, kProxyDirect), BM_GETCHECK, 0, 0) == BST_CHECKED) return Mode::direct;
    return Mode::automatic;
}

ProxyForm form_of(HWND h) {
    ProxyForm f;
    f.mode = mode_of(h);
    f.server = text_of(h, kProxyServerEdit);
    f.bypass = text_of(h, kProxyBypassEdit);
    f.pac_url = text_of(h, kProxyPacEdit);
    return f;
}

void enable_fields(Window& win) {
    const bool locked = win.state.locked;
    const Mode m = mode_of(win.hwnd);
    for (int id : {kProxyAuto, kProxyManual, kProxyPac, kProxyDirect}) EnableWindow(item(win.hwnd, id), !locked);
    EnableWindow(item(win.hwnd, kProxyServerEdit), !locked && m == Mode::manual);
    EnableWindow(item(win.hwnd, kProxyBypassEdit), !locked && m == Mode::manual);
    EnableWindow(item(win.hwnd, kProxyPacEdit), !locked && m == Mode::pac);
    EnableWindow(item(win.hwnd, kProxySaveButton), !locked);
}

void post_text(const std::shared_ptr<std::atomic<HWND>>& alive, UINT msg, int generation, std::string text) {
    HWND h = alive->load();
    if (!h) return;
    auto* p = new std::string(std::move(text));
    if (!PostMessageW(h, msg, static_cast<WPARAM>(generation), reinterpret_cast<LPARAM>(p))) delete p;
}

// The route for what the form says now (or the effective one when locked); on a worker thread.
void refresh_route(Window& win) {
    const ProxyForm f = form_of(win.hwnd);
    const int gen = ++win.route_generation;
    if (!win.state.locked && form_error(f)) {
        SetWindowTextW(item(win.hwnd, kProxyRouteText), w("Route: " + *form_error(f)).c_str());
        return;
    }
    SetWindowTextW(item(win.hwnd, kProxyRouteText), L"Route: determining...");
    const Setting s = win.state.locked ? win.state.setting : form_setting(f);
    std::thread([route = win.services.route, alive = win.alive, s, gen] {
        std::string text;
        try {
            text = route ? route(s) : std::string("not available");
        } catch (const std::exception& e) {
            text = std::string("cannot be determined: ") + e.what();
        }
        post_text(alive, kRouteMessage, gen, "Route: " + text);
    }).detach();
}

void start_test(Window& win) {
    const ProxyForm f = form_of(win.hwnd);
    if (!win.state.locked)
        if (auto e = form_error(f)) {
            SetWindowTextW(item(win.hwnd, kProxyTestText), w("Test: " + *e).c_str());
            return;
        }
    SetWindowTextW(item(win.hwnd, kProxyTestText), L"Test: connecting to the gateway (HTTPS and WebSocket)...");
    EnableWindow(item(win.hwnd, kProxyTestButton), FALSE);
    const Setting s = win.state.locked ? win.state.setting : form_setting(f);
    std::thread([test = win.services.test, alive = win.alive, s] {
        std::string text;
        try {
            const auto r = test ? test(s) : link::proxycfg::ProbeResult{};
            text = std::string(r.ok ? "Test: OK. " : "Test: FAILED. ") + r.summary;
        } catch (const std::exception& e) {
            text = std::string("Test: FAILED. ") + e.what();
        }
        post_text(alive, kTestMessage, 0, text);
    }).detach();
}

bool save(Window& win) {
    const ProxyForm f = form_of(win.hwnd);
    if (auto e = form_error(f)) {
        MessageBoxW(win.hwnd, w(*e).c_str(), L"fairyfly - Network / proxy", MB_OK | MB_ICONWARNING);
        return false;
    }
    const auto err = win.services.save ? win.services.save(form_setting(f)) : std::optional<std::string>("not available");
    if (err) {
        MessageBoxW(win.hwnd, w(*err).c_str(), L"fairyfly - Network / proxy", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (win.services.saved) win.services.saved();
    return true;
}

int scale(HWND h, int px) {
    const UINT dpi = h ? GetDpiForWindow(h) : GetDpiForSystem();
    return MulDiv(px, static_cast<int>(dpi ? dpi : 96), 96);
}

void create_controls(Window& win) {
    HWND h = win.hwnd;
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    int y = scale(h, 12);
    const int x = scale(h, 14), width = scale(h, 500), line = scale(h, 22), gap = scale(h, 6);
    auto add = [&](const wchar_t* cls, const std::wstring& text, DWORD style, int id, int cx, int cy, int dx = 0, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, x + dx, y, cx, cy, h,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(win.font), TRUE);
        return c;
    };
    if (win.state.locked) {
        add(L"STATIC", w(win.state.locked_text), SS_LEFT, kProxyLockText, width, line);
        y += line + gap;
    }
    add(L"BUTTON", L"&Automatic (detect: browser policies, Windows settings, HTTPS_PROXY)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, kProxyAuto, width, line);
    y += line;
    add(L"BUTTON", L"&Manual proxy server", BS_AUTORADIOBUTTON, kProxyManual, width, line);
    y += line;
    const int label = scale(h, 150), field = width - label - scale(h, 20);
    add(L"STATIC", L"Proxy (host:port):", SS_LEFT, -1, label, line, scale(h, 20));
    add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, kProxyServerEdit, field, line, scale(h, 20) + label);
    y += line + gap / 2;
    add(L"STATIC", L"Exceptions (; separated):", SS_LEFT, -1, label, line, scale(h, 20));
    add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, kProxyBypassEdit, field, line, scale(h, 20) + label);
    y += line + gap;
    add(L"BUTTON", L"&PAC script (automatic configuration)", BS_AUTORADIOBUTTON, kProxyPac, width, line);
    y += line;
    add(L"STATIC", L"Script address:", SS_LEFT, -1, label, line, scale(h, 20));
    add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, kProxyPacEdit, field, line, scale(h, 20) + label);
    y += line + gap;
    add(L"BUTTON", L"&Direct (no proxy)", BS_AUTORADIOBUTTON, kProxyDirect, width, line);
    y += line + gap;
    add(L"STATIC", w(link::proxycfg::kUnsupportedText), SS_LEFT, kProxyNoteText, width, line);
    y += line + gap;
    add(L"STATIC", L"Route:", SS_LEFT | SS_NOPREFIX, kProxyRouteText, width, line * 3);
    y += line * 3 + gap;
    add(L"STATIC", win.state.warnings.empty() ? L"Test: not run yet" : w("Note: " + win.state.warnings), SS_LEFT | SS_NOPREFIX,
        kProxyTestText, width, line * 4);
    y += line * 4 + gap;
    const int bw = scale(h, 130), bh = scale(h, 28);
    add(L"BUTTON", L"&Test connection", BS_PUSHBUTTON | WS_TABSTOP, kProxyTestButton, bw, bh);
    add(L"BUTTON", L"&Save", BS_DEFPUSHBUTTON | WS_TABSTOP, kProxySaveButton, scale(h, 90), bh, width - scale(h, 190));
    add(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, kProxyCancelButton, scale(h, 90), bh, width - scale(h, 90));
    y += bh + scale(h, 14);

    RECT r{0, 0, width + 2 * x, y};
    AdjustWindowRectExForDpi(&r, GetWindowLongW(h, GWL_STYLE), FALSE, GetWindowLongW(h, GWL_EXSTYLE), GetDpiForWindow(h));
    SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    const Setting& s = win.state.setting;
    const int radio = s.mode == Mode::manual ? kProxyManual : s.mode == Mode::pac ? kProxyPac : s.mode == Mode::direct ? kProxyDirect : kProxyAuto;
    SendMessageW(item(h, radio), BM_SETCHECK, BST_CHECKED, 0);
    SetWindowTextW(item(h, kProxyServerEdit), w(s.server).c_str());
    SetWindowTextW(item(h, kProxyBypassEdit), w(s.bypass).c_str());
    SetWindowTextW(item(h, kProxyPacEdit), w(s.pac_url).c_str());
    SendMessageW(item(h, kProxyServerEdit), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"proxy.corp.example:8080"));
    SendMessageW(item(h, kProxyBypassEdit), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"<local>;*.corp.example"));
    SendMessageW(item(h, kProxyPacEdit), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"https://pac.corp.example/proxy.pac"));
    enable_fields(win);
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* win = reinterpret_cast<Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        win = static_cast<Window*>(cs->lpCreateParams);
        win->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(win));
        break;
    }
    case WM_CREATE: {
        NONCLIENTMETRICSW m{};
        m.cbSize = sizeof m;
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0, GetDpiForWindow(h));
        win->font = CreateFontIndirectW(&m.lfMessageFont);
        win->alive->store(h);
        create_controls(*win);
        refresh_route(*win);
        return 0;
    }
    case WM_COMMAND: {
        if (!win) break;
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if ((id == kProxyAuto || id == kProxyManual || id == kProxyPac || id == kProxyDirect) && code == BN_CLICKED) {
            enable_fields(*win);
            refresh_route(*win);
        } else if ((id == kProxyServerEdit || id == kProxyBypassEdit || id == kProxyPacEdit) && code == EN_KILLFOCUS) {
            refresh_route(*win);
        } else if (id == kProxyTestButton && code == BN_CLICKED) {
            start_test(*win);
        } else if ((id == kProxySaveButton || id == IDOK) && code == BN_CLICKED) {
            if (!win->state.locked && save(*win)) DestroyWindow(h);
        } else if ((id == kProxyCancelButton || id == IDCANCEL) && code == BN_CLICKED) {
            DestroyWindow(h);
        }
        return 0;
    }
    case kRouteMessage: {
        std::unique_ptr<std::string> t(reinterpret_cast<std::string*>(lp));
        if (win && static_cast<int>(wp) == win->route_generation) SetWindowTextW(item(h, kProxyRouteText), w(*t).c_str());
        return 0;
    }
    case kTestMessage: {
        std::unique_ptr<std::string> t(reinterpret_cast<std::string*>(lp));
        if (win) {
            SetWindowTextW(item(h, kProxyTestText), w(*t).c_str());
            EnableWindow(item(h, kProxyTestButton), TRUE);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        // the lock line in the warning colour of the system (as the tray's notices word it)
        if (win && reinterpret_cast<HWND>(lp) == item(h, kProxyLockText)) {
            SetTextColor(reinterpret_cast<HDC>(wp), RGB(0xA0, 0x40, 0x00));
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        if (win) {
            win->alive->store(nullptr);
            win->modal_done = true;
        }
        return 0;
    case WM_NCDESTROY:  // after the children: their font can go now
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (win) {
            if (win->font) DeleteObject(win->font);
            win->font = nullptr;
            if (win->hwnd == h) win->hwnd = nullptr;
        }
        break;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool register_class() {
    static const bool ok = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = kClass;
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return ok;
}

HWND create(Window& win) {
    if (!register_class()) return nullptr;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kClass, L"fairyfly - Network / proxy", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             scale(nullptr, 560), scale(nullptr, 560), nullptr, nullptr, GetModuleHandleW(nullptr), &win);
    if (!h) return nullptr;
    // centred on the work area of the primary monitor (the tray icon's corner is not known here)
    RECT wr{}, area{};
    GetWindowRect(h, &wr);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    SetWindowPos(h, HWND_TOP, area.left + ((area.right - area.left) - (wr.right - wr.left)) / 2,
                 area.top + ((area.bottom - area.top) - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE);
    return h;
}

}  // namespace

void show_proxy_window(const ProxyWindowServices& services) {
    if (g_open && g_open->hwnd) {
        SetForegroundWindow(g_open->hwnd);
        return;
    }
    Window win;
    win.services = services;
    try {
        win.state = services.load ? services.load() : ProxyWindowState{};
    } catch (const std::exception& e) {
        win.state.warnings = e.what();
    }
    if (!create(win)) return;
    g_open = &win;
    ShowWindow(win.hwnd, SW_SHOW);
    SetForegroundWindow(win.hwnd);
    MSG m;
    while (!win.modal_done) {
        const BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got == 0) {  // the tray's Quit: close and pass it on to the tray's own loop
            if (win.hwnd) DestroyWindow(win.hwnd);
            PostQuitMessage(static_cast<int>(m.wParam));
            break;
        }
        if (got < 0) break;
        if (win.hwnd && IsDialogMessageW(win.hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_open = nullptr;
}

void* create_proxy_window(const ProxyWindowServices& services) {
    auto* win = new Window;  // owned by the test process until it ends (a test helper)
    win->services = services;
    win->state = services.load ? services.load() : ProxyWindowState{};
    HWND h = create(*win);
    if (!h) {
        delete win;
        return nullptr;
    }
    ShowWindow(h, SW_SHOWNOACTIVATE);
    UpdateWindow(h);
    return h;
}

}  // namespace fairyfly::tray
