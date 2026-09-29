#include "include/tray/tray_win32.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "include/com/utf8.h"
#include "include/config/mcp_config.h"
#include "tray_resource.h"

namespace fairyfly::tray {

namespace {

using com::utf8_to_wide;

constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kMsgRefresh = WM_APP + 2;
constexpr UINT kMsgBalloon = WM_APP + 3;
constexpr UINT kMsgQuit = WM_APP + 4;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTickMs = 2000;
constexpr int kFirstCommandId = 1000;

void copy_wide(wchar_t* dest, size_t capacity, const std::wstring& src) {
    wcsncpy_s(dest, capacity, src.c_str(), _TRUNCATE);
}

/// Draws a filled circle icon (fallback when the .rc resource is not linked, e.g. in test binaries).
HICON draw_circle_icon(COLORREF color) {
    const int n = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = n;
    bi.bV5Height = -n;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color_bmp = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color_bmp || !bits) return nullptr;
    auto* px = static_cast<DWORD*>(bits);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double dx = x - (n - 1) / 2.0, dy = y - (n - 1) / 2.0;
            const bool inside = dx * dx + dy * dy <= (n / 2.0 - 1) * (n / 2.0 - 1);
            px[y * n + x] = inside ? (0xFF000000u | (GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color)) : 0u;
        }
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color_bmp;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color_bmp);
    DeleteObject(mask);
    return icon;
}

HICON load_state_icon(IconState state) {
    int id = IDI_TRAY_STOPPED;
    COLORREF color = RGB(140, 140, 140);
    switch (state) {
    case IconState::Running: id = IDI_TRAY_RUNNING; color = RGB(46, 160, 67); break;
    case IconState::Warning: id = IDI_TRAY_WARNING; color = RGB(224, 168, 0); break;
    case IconState::Error: id = IDI_TRAY_ERROR; color = RGB(214, 48, 49); break;
    case IconState::Stopped: break;
    }
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!icon) icon = draw_circle_icon(color);
    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
    return icon;
}

// ---- Tray host ----------------------------------------------------------------------------------
class Win32TrayHost : public TrayHost {
public:
    bool run(TrayListener& listener, const std::function<void(bool)>& ready) override {
        listener_ = &listener;
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");

        WNDCLASSW wc{};
        wc.lpfnWndProc = &Win32TrayHost::wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"fairyfly_tray_window";
        RegisterClassW(&wc);
        // A real (hidden) top-level window: message-only windows do not receive the TaskbarCreated broadcast.
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"fairyfly MCP tray", WS_POPUP, 0, 0, 0, 0,
                                    nullptr, nullptr, wc.hInstance, this);
        if (!hwnd) {
            ready(false);
            return false;
        }
        hwnd_ = hwnd;
        if (!add_icon()) {
            DestroyWindow(hwnd);
            hwnd_ = nullptr;
            ready(false);
            return false;
        }
        SetTimer(hwnd, kTimerId, kTickMs, nullptr);
        ready(true);
        if (quit_requested_) PostMessageW(hwnd, kMsgQuit, 0, 0);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        hwnd_ = nullptr;
        for (auto& icon : icons_) icon = nullptr;
        return true;
    }

    void post_quit() override {
        quit_requested_ = true;
        if (HWND hwnd = hwnd_) PostMessageW(hwnd, kMsgQuit, 0, 0);
    }
    void refresh() override {
        if (HWND hwnd = hwnd_) PostMessageW(hwnd, kMsgRefresh, 0, 0);
    }
    void balloon(const std::string& title, const std::string& text) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            balloons_.emplace_back(title, text);
        }
        if (HWND hwnd = hwnd_) PostMessageW(hwnd, kMsgBalloon, 0, 0);
    }
    bool confirm(const std::string& title, const std::string& text) override {
        return MessageBoxW(hwnd_, utf8_to_wide(text).c_str(), utf8_to_wide(title).c_str(),
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND | MB_TOPMOST) == IDYES;
    }
    void message_box(const std::string& title, const std::string& text) override {
        MessageBoxW(hwnd_, utf8_to_wide(text).c_str(), utf8_to_wide(title).c_str(),
                    MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        Win32TrayHost* self = reinterpret_cast<Win32TrayHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
        return self->handle(hwnd, msg, wparam, lparam);
    }

    LRESULT handle(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == taskbar_created_ && taskbar_created_ != 0) {
            add_icon();   // Explorer restarted: the icon is gone, add it again
            return 0;
        }
        switch (msg) {
        case kTrayCallback:
            if (LOWORD(lparam) == WM_LBUTTONUP) {
                show_balloon("fairyfly MCP", listener_->left_click_text());
            } else if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
                show_menu(hwnd);
            }
            return 0;
        case WM_TIMER:
            if (wparam == kTimerId) {
                listener_->on_tick();
                update_icon();
            }
            return 0;
        case kMsgRefresh: update_icon(); return 0;
        case kMsgBalloon: {
            std::deque<std::pair<std::string, std::string>> pending;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending.swap(balloons_);
            }
            for (const auto& b : pending) show_balloon(b.first, b.second);
            return 0;
        }
        case kMsgQuit: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            remove_icon();
            PostQuitMessage(0);
            return 0;
        default: break;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    NOTIFYICONDATAW base_data() const {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        return nid;
    }

    HICON icon_for_state(IconState state) {
        HICON& slot = icons_[static_cast<size_t>(state)];
        if (!slot) slot = load_state_icon(state);
        return slot;
    }

    bool add_icon() {
        const TrayView view = listener_->view();
        NOTIFYICONDATAW nid = base_data();
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = kTrayCallback;
        nid.hIcon = icon_for_state(view.icon);
        copy_wide(nid.szTip, ARRAYSIZE(nid.szTip), utf8_to_wide(view.tooltip));
        current_icon_ = view.icon;
        current_tip_ = view.tooltip;
        return Shell_NotifyIconW(NIM_ADD, &nid) == TRUE;
    }

    void update_icon() {
        const TrayView view = listener_->view();
        if (view.icon == current_icon_ && view.tooltip == current_tip_) return;
        NOTIFYICONDATAW nid = base_data();
        nid.uFlags = NIF_ICON | NIF_TIP;
        nid.hIcon = icon_for_state(view.icon);
        copy_wide(nid.szTip, ARRAYSIZE(nid.szTip), utf8_to_wide(view.tooltip));
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        current_icon_ = view.icon;
        current_tip_ = view.tooltip;
    }

    void remove_icon() {
        NOTIFYICONDATAW nid = base_data();
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    void show_balloon(const std::string& title, const std::string& text) {
        NOTIFYICONDATAW nid = base_data();
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        copy_wide(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), utf8_to_wide(title));
        copy_wide(nid.szInfo, ARRAYSIZE(nid.szInfo), utf8_to_wide(text));
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    HMENU build_native(const std::vector<MenuItem>& items, std::vector<TrayAction>& ids) {
        HMENU menu = CreatePopupMenu();
        for (const auto& item : items) {
            if (item.separator) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            UINT flags = MF_STRING;
            if (!item.enabled) flags |= MF_GRAYED;
            if (item.checked) flags |= MF_CHECKED;
            const std::wstring label = utf8_to_wide(item.label);
            if (!item.children.empty()) {
                HMENU sub = build_native(item.children, ids);
                AppendMenuW(menu, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(sub), label.c_str());
            } else {
                ids.push_back(item.action);
                AppendMenuW(menu, flags, static_cast<UINT_PTR>(kFirstCommandId + ids.size() - 1), label.c_str());
            }
        }
        return menu;
    }

    void show_menu(HWND hwnd) {
        const TrayMenuModel model = listener_->menu();
        std::vector<TrayAction> ids;
        HMENU menu = build_native(model.items, ids);
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);   // required so the menu closes when clicking elsewhere
        const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                                          pt.x, pt.y, 0, hwnd, nullptr));
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (cmd >= static_cast<UINT>(kFirstCommandId) && cmd - kFirstCommandId < ids.size())
            listener_->on_action(ids[cmd - kFirstCommandId]);
    }

    TrayListener* listener_ = nullptr;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> quit_requested_{false};
    UINT taskbar_created_ = 0;
    HICON icons_[4] = {nullptr, nullptr, nullptr, nullptr};
    IconState current_icon_ = IconState::Stopped;
    std::string current_tip_;
    std::mutex mutex_;
    std::deque<std::pair<std::string, std::string>> balloons_;
};

// ---- Small OS helpers ---------------------------------------------------------------------------
class Win32FileOpener : public FileOpener {
public:
    bool open(const std::filesystem::path& path) override {
        const auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        return result > 32;
    }
};

std::wstring build_environment_block(const std::vector<std::pair<std::string, std::string>>& extra) {
    std::wstring block;
    wchar_t* env = GetEnvironmentStringsW();
    if (env) {
        for (const wchar_t* p = env; *p;) {
            const size_t len = wcslen(p);
            block.append(p, len + 1);
            p += len + 1;
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& kv : extra) block += utf8_to_wide(kv.first) + L"=" + utf8_to_wide(kv.second) + L'\0';
    block += L'\0';
    return block;
}

class Win32ProcessLauncher : public ProcessLauncher {
public:
    std::optional<std::uint32_t> launch(const LaunchRequest& request) override {
        std::wstring command = utf8_to_wide(request.command_line);
        command.push_back(L'\0');
        DWORD flags = CREATE_UNICODE_ENVIRONMENT;
        if (request.detached) flags |= CREATE_NO_WINDOW | DETACHED_PROCESS;
        if (request.new_console) flags |= CREATE_NEW_CONSOLE;
        std::wstring env;
        if (!request.extra_env.empty()) env = build_environment_block(request.extra_env);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags,
                            env.empty() ? nullptr : env.data(), nullptr, &si, &pi))
            return std::nullopt;
        const std::uint32_t pid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pid;
    }
};

class Win32RunKey : public RegistryRunKey {
public:
    bool set(const std::string& name, const std::string& command) override {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kSubKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
        const std::wstring value = utf8_to_wide(command);
        const LONG rc = RegSetValueExW(key, utf8_to_wide(name).c_str(), 0, REG_SZ,
                                       reinterpret_cast<const BYTE*>(value.c_str()),
                                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return rc == ERROR_SUCCESS;
    }
    bool remove(const std::string& name) override {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSubKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return true;   // nothing to remove
        const LONG rc = RegDeleteValueW(key, utf8_to_wide(name).c_str());
        RegCloseKey(key);
        return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    }
    std::optional<std::string> get(const std::string& name) override {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSubKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return std::nullopt;
        wchar_t buffer[2048];
        DWORD size = sizeof(buffer) - sizeof(wchar_t), type = 0;
        const LONG rc = RegQueryValueExW(key, utf8_to_wide(name).c_str(), nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size);
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS || type != REG_SZ) return std::nullopt;
        buffer[size / sizeof(wchar_t)] = L'\0';
        return com::wide_to_utf8(buffer);
    }

private:
    static constexpr const wchar_t* kSubKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
};

class Win32SingleInstance : public SingleInstance {
public:
    ~Win32SingleInstance() override {
        if (handle_) CloseHandle(handle_);
    }
    bool acquire() override {
        if (handle_) return true;
        HANDLE h = CreateMutexW(nullptr, FALSE, utf8_to_wide(kTrayMutexName).c_str());
        if (!h) return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(h);
            return false;
        }
        handle_ = h;
        return true;
    }
    bool exists() override {
        if (handle_) return false;
        HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, utf8_to_wide(kTrayMutexName).c_str());
        if (!h) return false;
        CloseHandle(h);
        return true;
    }

private:
    HANDLE handle_ = nullptr;
};

class Win32Clipboard : public Clipboard {
public:
    bool set_text(const std::string& text) override {
        const std::wstring wide = utf8_to_wide(text);
        if (!OpenClipboard(nullptr)) return false;
        EmptyClipboard();
        const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool ok = false;
        if (mem) {
            if (void* dst = GlobalLock(mem)) {
                memcpy(dst, wide.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);
        }
        CloseClipboard();
        return ok;
    }
};

} // namespace

std::unique_ptr<TrayHost> make_win32_tray_host() { return std::make_unique<Win32TrayHost>(); }
std::unique_ptr<FileOpener> make_win32_file_opener() { return std::make_unique<Win32FileOpener>(); }
std::unique_ptr<ProcessLauncher> make_win32_process_launcher() { return std::make_unique<Win32ProcessLauncher>(); }
std::unique_ptr<RegistryRunKey> make_win32_run_key() { return std::make_unique<Win32RunKey>(); }
std::unique_ptr<SingleInstance> make_win32_single_instance() { return std::make_unique<Win32SingleInstance>(); }
std::unique_ptr<Clipboard> make_win32_clipboard() { return std::make_unique<Win32Clipboard>(); }

std::string current_exe_path() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    return com::wide_to_utf8(std::wstring_view(buffer, n));
}

std::vector<std::string> current_args() {
    std::vector<std::string> out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return out;
    for (int i = 1; i < argc; ++i) out.push_back(com::wide_to_utf8(argv[i]));
    LocalFree(argv);
    return out;
}

bool process_has_console() { return GetConsoleWindow() != nullptr; }

bool is_tray_child_process() { return config::process_env()(kTrayChildEnv) == "1"; }

void detach_console() { FreeConsole(); }

std::string route_logging_to_file() {
    const auto dir = log_directory(config::process_env());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "mcp.log";
    try {
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(file.string(), 5 * 1024 * 1024, 3);
        auto logger = std::make_shared<spdlog::logger>("fairyfly", sink);
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);
    } catch (const std::exception&) {
    }
    return file.string();
}

} // namespace fairyfly::tray
